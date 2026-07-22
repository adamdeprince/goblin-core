#include "goblin/core/auth.hpp"
#include "goblin/core/ring_client.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <poll.h>
#if defined(__linux__)
#include <sched.h>
#endif
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace {

using namespace std::chrono_literals;
using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

struct Child {
  pid_t pid{-1};
  explicit Child(pid_t value) : pid(value) {}
  ~Child() {
    if (pid <= 0) return;
    (void)::kill(pid, SIGTERM);
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (::waitpid(pid, nullptr, WNOHANG) == pid) return;
      std::this_thread::sleep_for(10ms);
    }
    (void)::kill(pid, SIGKILL);
    (void)::waitpid(pid, nullptr, 0);
  }
  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
};

[[nodiscard]] Child spawn_server(const char* binary,
                                 const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    const int null_fd = ::open("/dev/null", O_WRONLY);
    if (null_fd >= 0) {
      (void)::dup2(null_fd, STDOUT_FILENO);
      (void)::dup2(null_fd, STDERR_FILENO);
    }
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(binary));
    for (const auto& arg : args) argv.push_back(const_cast<char*>(arg.c_str()));
    argv.push_back(nullptr);
    ::execv(binary, argv.data());
    _exit(127);
  }
  return Child(pid);
}

[[nodiscard]] std::uint16_t reserve_ipv4_port() {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  assert(::bind(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) == 0);
  socklen_t size = sizeof(address);
  assert(::getsockname(fd, reinterpret_cast<sockaddr*>(&address), &size) == 0);
  const auto port = ntohs(address.sin_port);
  (void)::close(fd);
  return port;
}

[[nodiscard]] std::uint16_t reserve_dual_stack_port() {
  for (;;) {
    const int ipv6 = ::socket(AF_INET6, SOCK_STREAM, 0);
    assert(ipv6 >= 0);
    int one = 1;
    assert(::setsockopt(ipv6, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) ==
           0);
    sockaddr_in6 address6{};
    address6.sin6_family = AF_INET6;
    address6.sin6_addr = in6addr_loopback;
    assert(::bind(ipv6, reinterpret_cast<const sockaddr*>(&address6),
                  sizeof(address6)) == 0);
    socklen_t size = sizeof(address6);
    assert(::getsockname(ipv6, reinterpret_cast<sockaddr*>(&address6), &size) ==
           0);
    const auto port = ntohs(address6.sin6_port);
    const int ipv4 = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(ipv4 >= 0);
    sockaddr_in address4{};
    address4.sin_family = AF_INET;
    address4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address4.sin_port = htons(port);
    const bool free = ::bind(ipv4, reinterpret_cast<const sockaddr*>(&address4),
                             sizeof(address4)) == 0;
    (void)::close(ipv4);
    (void)::close(ipv6);
    if (free) return port;
  }
}

[[nodiscard]] int connect_uds(const std::string& path) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    (void)::close(fd);
    return -1;
  }
  return fd;
}

[[nodiscard]] int wait_for_uds(const std::string& path) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (const int fd = connect_uds(path); fd >= 0) return fd;
    std::this_thread::sleep_for(10ms);
  }
  return -1;
}

void send_command(int fd, std::initializer_list<std::string_view> fields) {
  const auto wire = encode_command(
      std::span<const std::string_view>(fields.begin(), fields.size()));
  std::size_t offset = 0;
  while (offset < wire.size()) {
    const auto sent = ::send(fd, wire.data() + offset, wire.size() - offset, 0);
    assert(sent > 0);
    offset += static_cast<std::size_t>(sent);
  }
}

[[nodiscard]] std::string request(
    int fd, std::string& pending,
    std::initializer_list<std::string_view> fields) {
  send_command(fd, fields);
  const auto deadline = std::chrono::steady_clock::now() + 5s;
  for (;;) {
    if (const auto end = reply_end(pending)) {
      auto result = pending.substr(0, *end);
      pending.erase(0, *end);
      return result;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    assert(remaining.count() > 0);
    pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
    assert(::poll(&event, 1, static_cast<int>(remaining.count())) > 0);
    char bytes[4096];
    const auto received = ::recv(fd, bytes, sizeof(bytes), 0);
    assert(received > 0);
    pending.append(bytes, static_cast<std::size_t>(received));
  }
}

struct TestIdentity {
  std::string certificate;
  std::string private_key;

  explicit TestIdentity(std::string_view suffix) {
    certificate = "/tmp/goblin-repl-tls-" + std::string(suffix) + ".crt";
    private_key = "/tmp/goblin-repl-tls-" + std::string(suffix) + ".key";

    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    assert(key_context != nullptr);
    assert(EVP_PKEY_keygen_init(key_context) == 1);
    assert(EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) == 1);
    EVP_PKEY* key = nullptr;
    assert(EVP_PKEY_keygen(key_context, &key) == 1);
    EVP_PKEY_CTX_free(key_context);

    X509* cert = X509_new();
    assert(cert != nullptr);
    assert(X509_set_version(cert, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(cert), 1) == 1);
    assert(X509_gmtime_adj(X509_getm_notBefore(cert), 0) != nullptr);
    assert(X509_gmtime_adj(X509_getm_notAfter(cert), 3600) != nullptr);
    assert(X509_set_pubkey(cert, key) == 1);
    auto* subject = X509_get_subject_name(cert);
    assert(X509_NAME_add_entry_by_txt(
               subject, "CN", MBSTRING_ASC,
               reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
               0) == 1);
    assert(X509_set_issuer_name(cert, subject) == 1);
    assert(X509_sign(cert, key, EVP_sha256()) > 0);

    FILE* cert_file = std::fopen(certificate.c_str(), "w");
    assert(cert_file != nullptr && PEM_write_X509(cert_file, cert) == 1);
    assert(std::fclose(cert_file) == 0);
    FILE* key_file = std::fopen(private_key.c_str(), "w");
    assert(key_file != nullptr &&
           PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr,
                                nullptr) == 1);
    assert(std::fclose(key_file) == 0);
    X509_free(cert);
    EVP_PKEY_free(key);
  }

  ~TestIdentity() {
    (void)::unlink(certificate.c_str());
    (void)::unlink(private_key.c_str());
  }
};

}  // namespace

int main(int argc, char** argv) {
  using namespace goblin::core;
  if (argc != 2) {
    std::cerr << "usage: replication_tls_socket_test <goblin-core>\n";
    return 2;
  }

  const std::string suffix = std::to_string(::getpid());
  TestIdentity identity(suffix);
  const std::string primary_uds = "/tmp/goblin-repl-tls-p-" + suffix + ".sock";
  const std::string replica_uds = "/tmp/goblin-repl-tls-r-" + suffix + ".sock";
  const std::string auth = "/tmp/goblin-repl-tls-" + suffix + ".conf";
  const std::string password = "/tmp/goblin-repl-tls-" + suffix + ".password";
  for (const auto* path : {&primary_uds, &replica_uds, &auth, &password}) {
    (void)::unlink(path->c_str());
  }
  (void)::unlink((auth + ".lock").c_str());
  assert(upsert_auth_user(auth, "replicator", "secret") ==
         AuthUserUpdate::added);
  {
    std::ofstream out(password, std::ios::binary);
    out << "secret\n";
    assert(out.good());
  }

  {
    const auto primary_port = reserve_dual_stack_port();
    const auto replica_port = reserve_ipv4_port();
    std::vector<std::string> primary_args{
        "--auth-file", auth,
        "--listen", "[::]:" + std::to_string(primary_port),
        "--uds-listen", primary_uds,
        "--tls-cert-file", identity.certificate,
        "--tls-key-file", identity.private_key};
#if defined(__linux__)
    const int cpu = ::sched_getcpu();
    assert(cpu >= 0);
    primary_args.emplace_back("--cpu");
    primary_args.emplace_back(std::to_string(cpu));
#endif
    auto primary = spawn_server(argv[1], primary_args);
    const int primary_client = wait_for_uds(primary_uds);
    assert(primary_client >= 0);

    auto replica = spawn_server(
        argv[1], {"--auth-file", auth, "--port", std::to_string(replica_port),
                  "--unixsocket", replica_uds, "--replica-tcp", "::1",
                  std::to_string(primary_port), "--replica-tls",
                  "--replica-tls-ca-file", identity.certificate,
                  "--replica-tls-server-name", "localhost",
                  "--replica-auth-user", "replicator",
                  "--replica-auth-password-file", password});
    const int replica_client = wait_for_uds(replica_uds);
    assert(replica_client >= 0 && "TLS replica failed to start");

    std::string primary_pending;
    std::string replica_pending;
    assert(request(primary_client, primary_pending,
                   {"AUTH", "replicator", "secret"}) == "+OK\r\n");
    assert(request(replica_client, replica_pending,
                   {"AUTH", "replicator", "secret"}) == "+OK\r\n");
    assert(request(primary_client, primary_pending,
                   {"SET", "tls-replicated", "yes"}) == "+OK\r\n");
    for (int attempt = 0; attempt < 500; ++attempt) {
      if (request(replica_client, replica_pending, {"GET", "tls-replicated"}) ==
          "$3\r\nyes\r\n") {
        break;
      }
      assert(attempt != 499);
      std::this_thread::sleep_for(10ms);
    }
    assert(request(replica_client, replica_pending, {"ROLE"})
               .find("TLS TCP ::1") != std::string::npos);
    (void)::close(replica_client);
    (void)::close(primary_client);
  }

  for (const auto* path : {&primary_uds, &replica_uds, &auth, &password}) {
    (void)::unlink(path->c_str());
  }
  (void)::unlink((auth + ".lock").c_str());
  std::cout << "Authenticated TLS TCP replication OK\n";
  return 0;
}
