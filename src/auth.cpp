#include "goblin/core/auth.hpp"

#include <sodium.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace goblin::core {
namespace {

constexpr std::string_view kAuthHeader = "goblin-core-auth-v1";
constexpr std::size_t kMaxUsernameBytes = 256;

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

[[noreturn]] void fail_errno(std::string_view operation,
                             std::string_view path) {
  std::string message(operation);
  message.append(" '");
  message.append(path);
  message.append("': ");
  message.append(std::strerror(errno));
  throw std::runtime_error(std::move(message));
}

void initialize_sodium() {
  static const int initialized = sodium_init();
  if (initialized < 0) {
    fail("libsodium initialization failed");
  }
}

void validate_username(std::string_view username) {
  if (!valid_auth_username(username)) {
    fail("username must be 1-256 printable, non-whitespace ASCII bytes");
  }
}

void validate_password(std::string_view password) {
  if (password.empty()) {
    fail("password must not be empty");
  }
  if (password.size() > crypto_pwhash_PASSWD_MAX) {
    fail("password is too long for libsodium");
  }
}

void require_private_file(const struct stat& st, std::string_view path) {
  if (!S_ISREG(st.st_mode)) {
    fail("auth path is not a regular file");
  }
  if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    std::string message("auth file '");
    message.append(path);
    message.append("' must not be accessible by group or other users (use chmod 600)");
    throw std::runtime_error(std::move(message));
  }
  if (st.st_uid != ::geteuid()) {
    fail("auth file must be owned by the server user");
  }
}

[[nodiscard]] std::vector<AuthDatabase::Entry> read_entries(
    std::string_view path, bool allow_missing) {
  const std::string owned_path(path);
  int open_flags = O_RDONLY | O_CLOEXEC;
#ifdef O_NOFOLLOW
  open_flags |= O_NOFOLLOW;
#endif
  const int fd = ::open(owned_path.c_str(), open_flags);
  if (fd < 0) {
    if (allow_missing && errno == ENOENT) {
      return {};
    }
    fail_errno("cannot open auth file", path);
  }

  struct stat st {};
  if (::fstat(fd, &st) != 0) {
    const int saved_errno = errno;
    (void)::close(fd);
    errno = saved_errno;
    fail_errno("cannot stat auth file", path);
  }
  try {
    require_private_file(st, path);
  } catch (...) {
    (void)::close(fd);
    throw;
  }
  constexpr std::size_t kMaxAuthFileBytes = 16U * 1024U * 1024U;
  if (st.st_size < 0 || static_cast<std::uintmax_t>(st.st_size) >
                            kMaxAuthFileBytes) {
    (void)::close(fd);
    fail("auth file is too large");
  }

  std::string contents;
  contents.reserve(static_cast<std::size_t>(st.st_size));
  char buffer[4096];
  for (;;) {
    const auto count = ::read(fd, buffer, sizeof(buffer));
    if (count > 0) {
      if (contents.size() + static_cast<std::size_t>(count) >
          kMaxAuthFileBytes) {
        (void)::close(fd);
        fail("auth file is too large");
      }
      contents.append(buffer, static_cast<std::size_t>(count));
      continue;
    }
    if (count == 0) {
      break;
    }
    if (errno == EINTR) {
      continue;
    }
    const int saved_errno = errno;
    (void)::close(fd);
    errno = saved_errno;
    fail_errno("cannot read auth file", path);
  }
  if (::close(fd) != 0) {
    fail_errno("cannot close auth file", path);
  }

  std::size_t cursor = 0;
  const auto next_line = [&contents, &cursor]() -> std::optional<std::string_view> {
    if (cursor >= contents.size()) {
      return std::nullopt;
    }
    const auto newline = contents.find('\n', cursor);
    const auto end = newline == std::string::npos ? contents.size() : newline;
    std::string_view line(contents.data() + cursor, end - cursor);
    cursor = newline == std::string::npos ? contents.size() : newline + 1;
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    return line;
  };

  const auto header = next_line();
  if (!header || *header != kAuthHeader) {
    fail("auth file has an unsupported or missing format header");
  }

  std::vector<AuthDatabase::Entry> entries;
  std::size_t line_number = 1;
  while (const auto line = next_line()) {
    ++line_number;
    if (line->empty()) {
      continue;
    }
    const auto tab = line->find('\t');
    if (tab == std::string::npos ||
        line->find('\t', tab + 1) != std::string::npos) {
      throw std::runtime_error("malformed auth entry on line " +
                               std::to_string(line_number));
    }
    AuthDatabase::Entry entry{.username = std::string(line->substr(0, tab)),
                              .password_hash = std::string(line->substr(tab + 1))};
    validate_username(entry.username);
    if (entry.password_hash.empty() ||
        crypto_pwhash_str_needs_rehash(
            entry.password_hash.c_str(), crypto_pwhash_OPSLIMIT_INTERACTIVE,
            crypto_pwhash_MEMLIMIT_INTERACTIVE) < 0) {
      throw std::runtime_error("invalid password hash on line " +
                               std::to_string(line_number));
    }
    if (std::find_if(entries.begin(), entries.end(), [&](const auto& current) {
          return current.username == entry.username;
        }) != entries.end()) {
      throw std::runtime_error("duplicate auth username on line " +
                               std::to_string(line_number));
    }
    entries.push_back(std::move(entry));
  }
  return entries;
}

class FileLock {
 public:
  explicit FileLock(std::string_view path) {
    path_.assign(path);
    path_.append(".lock");
    fd_ = ::open(path_.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (fd_ < 0) {
      fail_errno("cannot open auth lock file", path_);
    }
    if (::flock(fd_, LOCK_EX) != 0) {
      const int saved_errno = errno;
      (void)::close(fd_);
      fd_ = -1;
      errno = saved_errno;
      fail_errno("cannot lock auth file", path_);
    }
  }

  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;

  ~FileLock() {
    if (fd_ >= 0) {
      (void)::flock(fd_, LOCK_UN);
      (void)::close(fd_);
    }
  }

 private:
  std::string path_;
  int fd_{-1};
};

void write_all(int fd, std::string_view bytes, std::string_view path) {
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    fail_errno("cannot write auth file", path);
  }
}

void write_entries(std::string_view path,
                   const std::vector<AuthDatabase::Entry>& entries) {
  const std::filesystem::path destination{std::string(path)};
  const auto parent = destination.has_parent_path()
                          ? destination.parent_path()
                          : std::filesystem::path(".");
  const auto base = destination.filename().string();
  const auto temporary =
      parent / ("." + base + ".tmp." + std::to_string(::getpid()) + "." +
                std::to_string(randombytes_random()));

  std::string contents;
  contents.reserve(kAuthHeader.size() + 1 + entries.size() * 128);
  contents.append(kAuthHeader);
  contents.push_back('\n');
  for (const auto& entry : entries) {
    contents.append(entry.username);
    contents.push_back('\t');
    contents.append(entry.password_hash);
    contents.push_back('\n');
  }

  const std::string temporary_string = temporary.string();
  int fd = ::open(temporary_string.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC,
                  0600);
  if (fd < 0) {
    fail_errno("cannot create temporary auth file", temporary_string);
  }
  try {
    write_all(fd, contents, temporary_string);
    if (::fsync(fd) != 0) {
      fail_errno("cannot sync temporary auth file", temporary_string);
    }
    if (::close(fd) != 0) {
      fd = -1;
      fail_errno("cannot close temporary auth file", temporary_string);
    }
    fd = -1;
    const std::string destination_string = destination.string();
    if (::rename(temporary_string.c_str(), destination_string.c_str()) != 0) {
      fail_errno("cannot replace auth file", destination_string);
    }

    const int directory_fd = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC);
    if (directory_fd >= 0) {
      (void)::fsync(directory_fd);
      (void)::close(directory_fd);
    }
  } catch (...) {
    if (fd >= 0) {
      (void)::close(fd);
    }
    (void)::unlink(temporary_string.c_str());
    throw;
  }
}

[[nodiscard]] std::string hash_password(std::string_view password) {
  initialize_sodium();
  validate_password(password);
  std::string hash(crypto_pwhash_STRBYTES, '\0');
  if (crypto_pwhash_str_alg(
          hash.data(), password.data(), static_cast<unsigned long long>(password.size()),
          crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
          crypto_pwhash_ALG_ARGON2ID13) != 0) {
    fail("libsodium could not allocate memory for password hashing");
  }
  hash.resize(std::strlen(hash.c_str()));
  return hash;
}

}  // namespace

bool valid_auth_username(std::string_view username) noexcept {
  if (username.empty() || username.size() > kMaxUsernameBytes) {
    return false;
  }
  return std::all_of(username.begin(), username.end(), [](unsigned char byte) {
    return byte >= 0x21 && byte <= 0x7e;
  });
}

void secure_zero_memory(void* data, std::size_t size) noexcept {
  if (data != nullptr && size != 0) {
    sodium_memzero(data, size);
  }
}

AuthDatabase::AuthDatabase(std::vector<Entry> entries)
    : entries_(std::move(entries)) {
  if (!entries_.empty()) {
    fallback_hash_ = entries_.front().password_hash;
  }
}

AuthDatabase AuthDatabase::load(std::string_view path) {
  initialize_sodium();
  auto entries = read_entries(path, false);
  if (entries.empty()) {
    fail("auth file must contain at least one user");
  }
  return AuthDatabase(std::move(entries));
}

bool AuthDatabase::verify(std::string_view username,
                          std::string_view password) const {
  initialize_sodium();
  const auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const auto& entry) {
                                    return entry.username == username;
                                  });
  const bool known = found != entries_.end();
  const std::string& hash = known ? found->password_hash : fallback_hash_;
  if (hash.empty()) {
    return false;
  }
  const int verified = crypto_pwhash_str_verify(
      hash.c_str(), password.data(), static_cast<unsigned long long>(password.size()));
  return known && verified == 0;
}

AuthUserUpdate upsert_auth_user(std::string_view path, std::string_view username,
                                std::string_view password) {
  initialize_sodium();
  validate_username(username);
  validate_password(password);
  FileLock lock(path);
  auto entries = read_entries(path, true);
  auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.username == username;
  });
  const auto update = found == entries.end() ? AuthUserUpdate::added
                                              : AuthUserUpdate::updated;
  std::string hash = hash_password(password);
  if (found == entries.end()) {
    entries.push_back(AuthDatabase::Entry{.username = std::string(username),
                                          .password_hash = std::move(hash)});
  } else {
    found->password_hash = std::move(hash);
  }
  std::sort(entries.begin(), entries.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.username < rhs.username;
  });
  write_entries(path, entries);
  return update;
}

void remove_auth_user(std::string_view path, std::string_view username,
                      std::string_view password) {
  initialize_sodium();
  validate_username(username);
  validate_password(password);
  FileLock lock(path);
  auto entries = read_entries(path, false);
  if (entries.empty()) {
    fail("auth file contains no users");
  }
  const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
    return entry.username == username;
  });
  const std::string& hash = found == entries.end() ? entries.front().password_hash
                                                   : found->password_hash;
  const bool verified = crypto_pwhash_str_verify(
                            hash.c_str(), password.data(),
                            static_cast<unsigned long long>(password.size())) == 0;
  if (found == entries.end() || !verified) {
    fail("username or password does not match");
  }
  if (entries.size() == 1) {
    fail("cannot remove the last auth user");
  }
  entries.erase(found);
  write_entries(path, entries);
}

}  // namespace goblin::core
