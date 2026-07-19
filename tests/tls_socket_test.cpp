#include "goblin/core/ring_client.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <netinet/in.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#undef NDEBUG
#include <cassert>

namespace {

using goblin::core::ring::encode_command;
using goblin::core::ring::reply_end;

int connect_ipv4(std::uint16_t port) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

int connect_ipv6(std::uint16_t port) {
  const int fd = ::socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) {
    return -1;
  }
  sockaddr_in6 address{};
  address.sin6_family = AF_INET6;
  address.sin6_port = htons(port);
  address.sin6_addr = in6addr_loopback;
  if (::connect(fd, reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0) {
    ::close(fd);
    return -1;
  }
  return fd;
}

std::uint16_t reserve_dual_stack_port() {
  for (;;) {
    const int ipv6 = ::socket(AF_INET6, SOCK_STREAM, 0);
    assert(ipv6 >= 0);
    int enabled = 1;
    assert(::setsockopt(ipv6, IPPROTO_IPV6, IPV6_V6ONLY, &enabled,
                        sizeof(enabled)) == 0);
    sockaddr_in6 address6{};
    address6.sin6_family = AF_INET6;
    address6.sin6_port = 0;
    address6.sin6_addr = in6addr_loopback;
    assert(::bind(ipv6, reinterpret_cast<const sockaddr*>(&address6),
                  sizeof(address6)) == 0);
    socklen_t length = sizeof(address6);
    assert(::getsockname(ipv6, reinterpret_cast<sockaddr*>(&address6),
                         &length) == 0);
    const auto port = ntohs(address6.sin6_port);

    const int ipv4 = ::socket(AF_INET, SOCK_STREAM, 0);
    assert(ipv4 >= 0);
    sockaddr_in address4{};
    address4.sin_family = AF_INET;
    address4.sin_port = htons(port);
    address4.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool available =
        ::bind(ipv4, reinterpret_cast<const sockaddr*>(&address4),
               sizeof(address4)) == 0;
    ::close(ipv4);
    ::close(ipv6);
    if (available) {
      return port;
    }
  }
}

template <class Connect>
int wait_for_connection(Connect&& connect) {
  for (int attempt = 0; attempt < 500; ++attempt) {
    if (const int fd = std::invoke(connect); fd >= 0) {
      return fd;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return -1;
}

void send_plain(int fd, std::span<const std::string_view> args) {
  const auto wire = encode_command(args);
  std::size_t offset = 0;
  while (offset < wire.size()) {
    const auto sent = ::send(fd, wire.data() + offset, wire.size() - offset, 0);
    assert(sent > 0);
    offset += static_cast<std::size_t>(sent);
  }
}

std::string read_plain_reply(int fd, std::string& pending) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    if (const auto end = reply_end(pending)) {
      auto reply = pending.substr(0, *end);
      pending.erase(0, *end);
      return reply;
    }
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    assert(remaining.count() > 0);
    pollfd event{.fd = fd, .events = POLLIN, .revents = 0};
    assert(::poll(&event, 1, static_cast<int>(remaining.count())) > 0);
    char buffer[8192];
    const auto received = ::recv(fd, buffer, sizeof(buffer), 0);
    assert(received > 0);
    pending.append(buffer, static_cast<std::size_t>(received));
  }
}

struct TlsClient {
  SSL_CTX* context{nullptr};
  SSL* connection{nullptr};
  int fd{-1};

  TlsClient() = default;

  ~TlsClient() {
    if (connection != nullptr) {
      SSL_free(connection);
    }
    if (context != nullptr) {
      SSL_CTX_free(context);
    }
    if (fd >= 0) {
      ::close(fd);
    }
  }

  TlsClient(const TlsClient&) = delete;
  TlsClient& operator=(const TlsClient&) = delete;
};

std::unique_ptr<TlsClient> connect_tls_ipv6(std::uint16_t port) {
  auto client = std::make_unique<TlsClient>();
  client->fd = connect_ipv6(port);
  if (client->fd < 0) {
    return nullptr;
  }
  client->context = SSL_CTX_new(TLS_client_method());
  if (client->context == nullptr) {
    return nullptr;
  }
  SSL_CTX_set_verify(client->context, SSL_VERIFY_NONE, nullptr);
  client->connection = SSL_new(client->context);
  if (client->connection == nullptr ||
      SSL_set_fd(client->connection, client->fd) != 1 ||
      SSL_connect(client->connection) != 1) {
    return nullptr;
  }
  return client;
}

void send_tls(TlsClient& client, std::span<const std::string_view> args) {
  const auto wire = encode_command(args);
  std::size_t offset = 0;
  while (offset < wire.size()) {
    std::size_t sent = 0;
    assert(SSL_write_ex(client.connection, wire.data() + offset,
                        wire.size() - offset, &sent) == 1);
    assert(sent > 0);
    offset += sent;
  }
}

std::string read_tls_reply(TlsClient& client, std::string& pending) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(5);
  for (;;) {
    if (const auto end = reply_end(pending)) {
      auto reply = pending.substr(0, *end);
      pending.erase(0, *end);
      return reply;
    }
    if (SSL_pending(client.connection) == 0) {
      const auto remaining =
          std::chrono::duration_cast<std::chrono::milliseconds>(
              deadline - std::chrono::steady_clock::now());
      assert(remaining.count() > 0);
      pollfd event{.fd = client.fd, .events = POLLIN, .revents = 0};
      assert(::poll(&event, 1, static_cast<int>(remaining.count())) > 0);
    }
    char buffer[8192];
    std::size_t received = 0;
    assert(SSL_read_ex(client.connection, buffer, sizeof(buffer), &received) ==
           1);
    assert(received > 0);
    pending.append(buffer, received);
  }
}

struct Child {
  pid_t pid{-1};

  explicit Child(pid_t child) : pid(child) {}
  ~Child() {
    if (pid > 0) {
      (void)::kill(pid, SIGTERM);
      (void)::waitpid(pid, nullptr, 0);
    }
  }

  Child(const Child&) = delete;
  Child& operator=(const Child&) = delete;
};

struct TestIdentity {
  std::string certificate_path;
  std::string private_key_path;

  TestIdentity() {
    const auto suffix = std::to_string(::getpid());
    certificate_path = "/tmp/goblin-tls-test-" + suffix + ".crt";
    private_key_path = "/tmp/goblin-tls-test-" + suffix + ".key";

    EVP_PKEY_CTX* key_context = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    assert(key_context != nullptr);
    assert(EVP_PKEY_keygen_init(key_context) == 1);
    assert(EVP_PKEY_CTX_set_rsa_keygen_bits(key_context, 2048) == 1);
    EVP_PKEY* key = nullptr;
    assert(EVP_PKEY_keygen(key_context, &key) == 1);
    EVP_PKEY_CTX_free(key_context);

    X509* certificate = X509_new();
    assert(certificate != nullptr);
    assert(X509_set_version(certificate, 2) == 1);
    assert(ASN1_INTEGER_set(X509_get_serialNumber(certificate), 1) == 1);
    assert(X509_gmtime_adj(X509_getm_notBefore(certificate), 0) != nullptr);
    assert(X509_gmtime_adj(X509_getm_notAfter(certificate), 3600) != nullptr);
    assert(X509_set_pubkey(certificate, key) == 1);
    X509_NAME* subject = X509_get_subject_name(certificate);
    assert(subject != nullptr);
    assert(X509_NAME_add_entry_by_txt(
               subject, "CN", MBSTRING_ASC,
               reinterpret_cast<const unsigned char*>("localhost"), -1, -1,
               0) == 1);
    assert(X509_set_issuer_name(certificate, subject) == 1);
    assert(X509_sign(certificate, key, EVP_sha256()) > 0);

    FILE* certificate_file = std::fopen(certificate_path.c_str(), "w");
    assert(certificate_file != nullptr);
    assert(PEM_write_X509(certificate_file, certificate) == 1);
    assert(std::fclose(certificate_file) == 0);

    FILE* key_file = std::fopen(private_key_path.c_str(), "w");
    assert(key_file != nullptr);
    assert(PEM_write_PrivateKey(key_file, key, nullptr, nullptr, 0, nullptr,
                                nullptr) == 1);
    assert(std::fclose(key_file) == 0);

    X509_free(certificate);
    EVP_PKEY_free(key);
  }

  ~TestIdentity() {
    (void)::unlink(certificate_path.c_str());
    (void)::unlink(private_key_path.c_str());
  }

  TestIdentity(const TestIdentity&) = delete;
  TestIdentity& operator=(const TestIdentity&) = delete;
};

Child spawn_server(const char* binary, const std::vector<std::string>& args) {
  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    std::vector<char*> argv;
    argv.reserve(args.size() + 2);
    argv.push_back(const_cast<char*>(binary));
    for (const auto& arg : args) {
      argv.push_back(const_cast<char*>(arg.c_str()));
    }
    argv.push_back(nullptr);
    ::execv(binary, argv.data());
    _exit(127);
  }
  return Child(pid);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "usage: tls_socket_test <goblin-core>\n";
    return 2;
  }

  TestIdentity identity;
  const auto port = reserve_dual_stack_port();
  auto server = spawn_server(
      argv[1], {"--listen", "[::]:" + std::to_string(port),
                "--tls-cert-file", identity.certificate_path,
                "--tls-key-file", identity.private_key_path});

  const int local =
      wait_for_connection([&] { return connect_ipv4(port); });
  assert(local >= 0 && "plaintext localhost listener was not created");
  std::string local_pending;
  const std::vector<std::string_view> ping{"PING"};
  send_plain(local, ping);
  assert(read_plain_reply(local, local_pending) == "+PONG\r\n");

  auto tls = connect_tls_ipv6(port);
  assert(tls != nullptr && "IPv6 TLS listener did not complete a handshake");
  std::string tls_pending;
  send_tls(*tls, ping);
  assert(read_tls_reply(*tls, tls_pending) == "+PONG\r\n");

  const std::vector<std::string_view> set_shared{"SET", "transport", "shared"};
  send_plain(local, set_shared);
  assert(read_plain_reply(local, local_pending) == "+OK\r\n");
  const std::vector<std::string_view> get_shared{"GET", "transport"};
  send_tls(*tls, get_shared);
  assert(read_tls_reply(*tls, tls_pending) == "$6\r\nshared\r\n");

  const std::string large(60'000, 'x');
  const std::vector<std::string_view> set_large{"SET", "large", large};
  send_plain(local, set_large);
  assert(read_plain_reply(local, local_pending) == "+OK\r\n");
  const std::vector<std::string_view> get_large{"GET", "large"};
  send_tls(*tls, get_large);
  assert(read_tls_reply(*tls, tls_pending) ==
         "$60000\r\n" + large + "\r\n");

  // Stop reading long enough for the nonblocking server socket to fill, then
  // verify that every partial TLS write resumes at the exact reply boundary.
  constexpr int pipeline_depth = 24;
  for (int i = 0; i < pipeline_depth; ++i) {
    send_tls(*tls, get_large);
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const std::string expected_large = "$60000\r\n" + large + "\r\n";
  for (int i = 0; i < pipeline_depth; ++i) {
    assert(read_tls_reply(*tls, tls_pending) == expected_large);
  }

  ::close(local);
  std::cout << "plaintext localhost plus IPv6 TLS listener OK\n";
  return 0;
}
