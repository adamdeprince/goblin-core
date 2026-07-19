#include "goblin/core/auth.hpp"

#include <exception>
#include <iostream>
#include <string_view>

namespace {

void usage(std::string_view program) {
  std::cerr << "usage: " << program
            << " [--file PATH] add|remove USERNAME PASSWORD\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string_view path = "goblin-core.auth";
  int offset = 1;
  if (argc >= 3 && std::string_view(argv[1]) == "--file") {
    path = argv[2];
    offset = 3;
  }
  if (argc - offset != 3) {
    usage(argv[0]);
    return 2;
  }

  const std::string_view operation(argv[offset]);
  const std::string_view username(argv[offset + 1]);
  const std::string_view password(argv[offset + 2]);
  struct PasswordScrubber {
    std::string_view password;
    ~PasswordScrubber() {
      goblin::core::secure_zero_memory(
          const_cast<char*>(password.data()), password.size());
    }
  } scrubber{password};
  try {
    if (operation == "add") {
      const auto result = goblin::core::upsert_auth_user(path, username, password);
      std::cout << (result == goblin::core::AuthUserUpdate::added ? "added "
                                                                  : "updated ")
                << username << " in " << path << '\n';
      return 0;
    }
    if (operation == "remove") {
      goblin::core::remove_auth_user(path, username, password);
      std::cout << "removed " << username << " from " << path << '\n';
      return 0;
    }
    usage(argv[0]);
    return 2;
  } catch (const std::exception& error) {
    std::cerr << "goblin-core-auth: " << error.what() << '\n';
    return 1;
  }
}
