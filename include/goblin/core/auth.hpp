#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace goblin::core {

class AuthDatabase {
 public:
  struct Entry {
    std::string username;
    std::string password_hash;
  };

  [[nodiscard]] static AuthDatabase load(std::string_view path);

  [[nodiscard]] bool verify(std::string_view username,
                            std::string_view password) const;
  [[nodiscard]] std::size_t size() const noexcept { return entries_.size(); }

 private:
  explicit AuthDatabase(std::vector<Entry> entries);

  std::vector<Entry> entries_;
  std::string fallback_hash_;
};

enum class AuthUserUpdate { added, updated };

// Adds a user or replaces that user's password hash. The file is rewritten
// atomically with mode 0600.
[[nodiscard]] AuthUserUpdate upsert_auth_user(std::string_view path,
                                              std::string_view username,
                                              std::string_view password);

// Removes a user only after verifying the supplied password. The last user
// cannot be removed because an auth-enabled server rejects an empty database.
void remove_auth_user(std::string_view path, std::string_view username,
                      std::string_view password);

[[nodiscard]] bool valid_auth_username(std::string_view username) noexcept;

// Best-effort non-optimizable clearing for transient protocol/argv buffers.
void secure_zero_memory(void* data, std::size_t size) noexcept;

}  // namespace goblin::core
