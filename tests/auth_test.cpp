#include "goblin/core/auth.hpp"

#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <exception>
#include <string>

#include <sys/stat.h>
#include <unistd.h>

namespace {

template <class Fn>
bool throws(Fn&& function) {
  try {
    function();
    return false;
  } catch (const std::exception&) {
    return true;
  }
}

}  // namespace

int main() {
  const std::string path =
      "/tmp/goblin-auth-unit-" + std::to_string(::getpid()) + ".conf";
  const std::string lock_path = path + ".lock";
  const std::string symlink_path = path + ".link";
  (void)::unlink(path.c_str());
  (void)::unlink(lock_path.c_str());
  (void)::unlink(symlink_path.c_str());

  assert(goblin::core::upsert_auth_user(path, "default", "first-secret") ==
         goblin::core::AuthUserUpdate::added);
  struct stat st {};
  assert(::stat(path.c_str(), &st) == 0);
  assert((st.st_mode & 0777) == 0600);

  auto auth = goblin::core::AuthDatabase::load(path);
  assert(auth.size() == 1);
  assert(auth.verify("default", "first-secret"));
  assert(!auth.verify("default", "wrong"));
  assert(!auth.verify("missing", "first-secret"));

  assert(goblin::core::upsert_auth_user(path, "service", "service-secret") ==
         goblin::core::AuthUserUpdate::added);
  assert(goblin::core::upsert_auth_user(path, "default", "rotated-secret") ==
         goblin::core::AuthUserUpdate::updated);
  auth = goblin::core::AuthDatabase::load(path);
  assert(auth.size() == 2);
  assert(!auth.verify("default", "first-secret"));
  assert(auth.verify("default", "rotated-secret"));

  assert(throws([&] {
    goblin::core::remove_auth_user(path, "service", "wrong");
  }));
  goblin::core::remove_auth_user(path, "service", "service-secret");
  assert(throws([&] {
    goblin::core::remove_auth_user(path, "default", "rotated-secret");
  }));

  assert(::chmod(path.c_str(), 0644) == 0);
  assert(throws([&] { (void)goblin::core::AuthDatabase::load(path); }));
  assert(::chmod(path.c_str(), 0600) == 0);
  assert(::symlink(path.c_str(), symlink_path.c_str()) == 0);
  assert(throws([&] { (void)goblin::core::AuthDatabase::load(symlink_path); }));

  assert(!goblin::core::valid_auth_username(""));
  assert(!goblin::core::valid_auth_username("contains space"));
  assert(goblin::core::valid_auth_username("worker-01"));

  (void)::unlink(path.c_str());
  (void)::unlink(lock_path.c_str());
  (void)::unlink(symlink_path.c_str());
  std::puts("auth file tests OK");
  return 0;
}
