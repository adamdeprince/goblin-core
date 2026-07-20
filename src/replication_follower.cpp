#include "replication_follower.hpp"

#include "goblin/core/rdma_client.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/sbe_socket_transport.hpp"
#include "pubsub.hpp"

#include <chrono>
#include <cerrno>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <thread>

#ifdef GOBLIN_HAS_TLS
#include <arpa/inet.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>
#include <poll.h>
#include <unistd.h>
#endif

namespace goblin::core {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

#ifdef GOBLIN_HAS_TLS
class ReplicaTlsSocket {
 public:
  ReplicaTlsSocket(const ReplicaTlsSocket&) = delete;
  ReplicaTlsSocket& operator=(const ReplicaTlsSocket&) = delete;

  ~ReplicaTlsSocket() {
    if (ssl_ != nullptr) SSL_free(ssl_);
    if (context_ != nullptr) SSL_CTX_free(context_);
    if (fd_ >= 0) (void)::close(fd_);
  }

  [[nodiscard]] static std::unique_ptr<ReplicaTlsSocket> open(
      const ReplicaTcpConfig& source, std::string& error) {
    auto plain = SocketSbeTransport::open(
        SbeSocketEndpoint::tcp(source.address, source.port), 5s, 64 * 1024,
        &error);
    if (!plain) return nullptr;

    auto result = std::unique_ptr<ReplicaTlsSocket>(new ReplicaTlsSocket);
    result->fd_ = plain->release_native_handle();
    ERR_clear_error();
    result->context_ = SSL_CTX_new(TLS_client_method());
    if (result->context_ == nullptr) {
      error = tls_error("could not create TLS client context");
      return nullptr;
    }
    if (SSL_CTX_set_min_proto_version(result->context_, TLS1_2_VERSION) != 1) {
      error = tls_error("could not require TLS 1.2 or newer");
      return nullptr;
    }
    SSL_CTX_set_options(result->context_, SSL_OP_NO_COMPRESSION);
    SSL_CTX_set_verify(result->context_, SSL_VERIFY_PEER, nullptr);
    if (!source.tls->ca_file.empty()) {
      if (SSL_CTX_load_verify_locations(result->context_,
                                        source.tls->ca_file.c_str(), nullptr) !=
          1) {
        error = tls_error("could not load replica TLS CA file");
        return nullptr;
      }
    } else if (SSL_CTX_set_default_verify_paths(result->context_) != 1) {
      error = tls_error("could not load the default TLS trust store");
      return nullptr;
    }

    result->ssl_ = SSL_new(result->context_);
    if (result->ssl_ == nullptr || SSL_set_fd(result->ssl_, result->fd_) != 1) {
      error = tls_error("could not initialize replica TLS session");
      return nullptr;
    }
    const std::string identity = source.tls->server_name.empty()
                                     ? source.address
                                     : source.tls->server_name;
    in_addr ipv4{};
    in6_addr ipv6{};
    auto* verify = SSL_get0_param(result->ssl_);
    if (::inet_pton(AF_INET, identity.c_str(), &ipv4) == 1 ||
        ::inet_pton(AF_INET6, identity.c_str(), &ipv6) == 1) {
      if (X509_VERIFY_PARAM_set1_ip_asc(verify, identity.c_str()) != 1) {
        error = "could not configure replica TLS IP verification";
        return nullptr;
      }
    } else {
      if (SSL_set1_host(result->ssl_, identity.c_str()) != 1 ||
          SSL_set_tlsext_host_name(result->ssl_, identity.c_str()) != 1) {
        error = tls_error("could not configure replica TLS hostname verification");
        return nullptr;
      }
    }

    const auto deadline = Clock::now() + 5s;
    for (;;) {
      ERR_clear_error();
      const int connected = SSL_connect(result->ssl_);
      if (connected == 1) break;
      const int ssl_error = SSL_get_error(result->ssl_, connected);
      if (ssl_error != SSL_ERROR_WANT_READ &&
          ssl_error != SSL_ERROR_WANT_WRITE) {
        error = tls_error("replica TLS handshake failed");
        return nullptr;
      }
      if (!wait_fd(result->fd_, ssl_error == SSL_ERROR_WANT_READ ? POLLIN
                                                                 : POLLOUT,
                   deadline, error)) {
        return nullptr;
      }
    }
    if (SSL_get_verify_result(result->ssl_) != X509_V_OK) {
      error = "replica TLS certificate verification failed: " +
              std::string(X509_verify_cert_error_string(
                  SSL_get_verify_result(result->ssl_)));
      return nullptr;
    }
    return result;
  }

  template <class StopFn>
  [[nodiscard]] bool send(std::string_view bytes, StopFn&& stop) {
    while (!bytes.empty()) {
      std::size_t sent = 0;
      ERR_clear_error();
      const int result = SSL_write_ex(ssl_, bytes.data(), bytes.size(), &sent);
      if (result == 1) {
        bytes.remove_prefix(sent);
        continue;
      }
      const int ssl_error = SSL_get_error(ssl_, result);
      if (ssl_error == SSL_ERROR_WANT_READ ||
          ssl_error == SSL_ERROR_WANT_WRITE) {
        if (stop()) return false;
        pollfd event{.fd = fd_,
                     .events = static_cast<short>(
                         ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT),
                     .revents = 0};
        (void)::poll(&event, 1, 0);
        ring::cpu_relax();
        continue;
      }
      throw std::runtime_error(tls_error("replica TLS write failed"));
    }
    return true;
  }

  [[nodiscard]] std::optional<std::string_view> peek() {
    if (consumed_ < inbound_.size()) {
      return std::string_view(inbound_).substr(consumed_);
    }
    inbound_.clear();
    consumed_ = 0;
    char bytes[16 * 1024];
    std::size_t received = 0;
    ERR_clear_error();
    const int result = SSL_read_ex(ssl_, bytes, sizeof(bytes), &received);
    if (result == 1) {
      inbound_.append(bytes, received);
      return std::string_view(inbound_);
    }
    const int ssl_error = SSL_get_error(ssl_, result);
    if (ssl_error == SSL_ERROR_WANT_READ ||
        ssl_error == SSL_ERROR_WANT_WRITE) {
      return std::nullopt;
    }
    if (ssl_error == SSL_ERROR_ZERO_RETURN) {
      throw std::runtime_error("upstream TLS firehose closed");
    }
    throw std::runtime_error(tls_error("replica TLS read failed"));
  }

  void pop() noexcept { consumed_ = inbound_.size(); }
  [[nodiscard]] int native_handle() const noexcept { return fd_; }

 private:
  ReplicaTlsSocket() = default;

  [[nodiscard]] static std::string tls_error(std::string_view operation) {
    std::string result(operation);
    const auto code = ERR_peek_last_error();
    if (code != 0) {
      char detail[256];
      ERR_error_string_n(code, detail, sizeof(detail));
      result.append(": ");
      result.append(detail);
    }
    return result;
  }

  [[nodiscard]] static bool wait_fd(
      int fd, short events, Clock::time_point deadline, std::string& error) {
    for (;;) {
      const auto now = Clock::now();
      if (now >= deadline) {
        error = "replica TLS handshake timed out";
        return false;
      }
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
          deadline - now);
      pollfd event{.fd = fd, .events = events, .revents = 0};
      const int ready = ::poll(&event, 1, static_cast<int>(remaining.count()));
      if (ready > 0) return true;
      if (ready < 0 && errno == EINTR) continue;
      if (ready < 0) {
        error = "replica TLS poll failed: " + std::string(std::strerror(errno));
        return false;
      }
    }
  }

  SSL_CTX* context_{nullptr};
  SSL* ssl_{nullptr};
  int fd_{-1};
  std::string inbound_;
  std::size_t consumed_{0};
};
#endif

}  // namespace

struct ReplicationFollowerRuntime::Impl {
  std::unique_ptr<ring::RingClient> ring_client;
  std::unique_ptr<SocketSbeTransport> socket_client;
#ifdef GOBLIN_HAS_TLS
  std::unique_ptr<ReplicaTlsSocket> tls_client;
#endif
#ifdef GOBLIN_HAS_RDMA
  std::unique_ptr<rdma::RdmaClient> rdma_client;
#endif
  detail::UnsolicitedOutputQueue buffered;
  std::string socket_pending;
  std::string description;
  FirehoseHello hello;
  std::uint64_t next_buffer_sequence{1};

  explicit Impl(std::size_t buffer_bytes) : buffered(buffer_bytes) {}

  void send(std::string_view bytes) {
    if (ring_client) {
      ring_client->send_raw(bytes);
      return;
    }
    if (socket_client) {
      const auto deadline = Clock::now() + 5s;
      if (!socket_client->send(bytes, [&] { return Clock::now() >= deadline; })) {
        throw std::runtime_error("firehose request timed out");
      }
      return;
    }
#ifdef GOBLIN_HAS_TLS
    if (tls_client) {
      const auto deadline = Clock::now() + 5s;
      if (!tls_client->send(bytes, [&] { return Clock::now() >= deadline; })) {
        throw std::runtime_error("firehose TLS request timed out");
      }
      return;
    }
#endif
#ifdef GOBLIN_HAS_RDMA
    rdma_client->send_raw(bytes);
#else
    throw std::runtime_error("RDMA replication is unavailable in this build");
#endif
  }

  [[nodiscard]] std::optional<std::string> try_read_socket() {
    for (;;) {
      if (const auto end = ring::reply_end(socket_pending)) {
        std::string result = socket_pending.substr(0, *end);
        socket_pending.erase(0, *end);
        return result;
      }
      std::optional<std::string_view> bytes;
      if (socket_client) {
        bytes = socket_client->peek();
      }
#ifdef GOBLIN_HAS_TLS
      else {
        bytes = tls_client->peek();
      }
#endif
      if (!bytes) return std::nullopt;
      socket_pending.append(*bytes);
      if (socket_client) {
        socket_client->pop();
      }
#ifdef GOBLIN_HAS_TLS
      else {
        tls_client->pop();
      }
#endif
    }
  }

  [[nodiscard]] std::optional<std::string> try_read() {
    if (ring_client) return ring_client->try_read_reply();
    if (socket_client
#ifdef GOBLIN_HAS_TLS
        || tls_client
#endif
    ) {
      return try_read_socket();
    }
#ifdef GOBLIN_HAS_RDMA
    auto result = rdma_client->try_read_reply();
    if (!result && (rdma_client->transport().connection().failed() ||
                    rdma_client->transport().connection().disconnected())) {
      std::string message = "upstream RDMA firehose closed";
      const auto detail = rdma_client->transport().connection().error();
      if (!detail.empty()) message += ": " + std::string(detail);
      throw std::runtime_error(message);
    }
    return result;
#else
    return std::nullopt;
#endif
  }

  [[nodiscard]] std::string read(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    unsigned spins = 0;
    for (;;) {
      if (auto frame = try_read()) return std::move(*frame);
      if ((++spins & 63U) == 0 && Clock::now() >= deadline) {
        throw std::runtime_error("upstream firehose response timed out");
      }
      if (socket_client
#ifdef GOBLIN_HAS_TLS
          || tls_client
#endif
      ) {
        std::this_thread::sleep_for(50us);
      } else {
        ring::cpu_relax();
      }
    }
  }
};

ReplicationFollowerRuntime::ReplicationFollowerRuntime(
    const ReplicaSourceConfig& config, std::size_t buffer_bytes,
    const ReplicaAuthConfig* auth)
    : impl_(std::make_unique<Impl>(buffer_bytes)) {
  if (const auto* source = std::get_if<ReplicaRingConfig>(&config)) {
    auto client = ring::RingClient::open(source->path.c_str(), 5s);
    if (!client) {
      throw std::runtime_error("could not connect to upstream ring " +
                               source->path);
    }
    impl_->ring_client =
        std::make_unique<ring::RingClient>(std::move(*client));
    impl_->description = "ring " + source->path;
  } else if (const auto* source = std::get_if<ReplicaUdsConfig>(&config)) {
    std::string error;
    auto transport = SocketSbeTransport::open(
        SbeSocketEndpoint::unix_domain(source->path), 5s, 64 * 1024, &error);
    if (!transport) {
      throw std::runtime_error("could not connect to upstream Unix socket " +
                               source->path +
                               (error.empty() ? "" : ": " + error));
    }
    impl_->socket_client =
        std::make_unique<SocketSbeTransport>(std::move(*transport));
    impl_->description = "Unix socket " + source->path;
  } else if (const auto* source = std::get_if<ReplicaTcpConfig>(&config)) {
    std::string error;
    if (source->tls) {
#ifdef GOBLIN_HAS_TLS
      impl_->tls_client = ReplicaTlsSocket::open(*source, error);
      if (!impl_->tls_client) {
        throw std::runtime_error("could not connect to upstream TLS TCP " +
                                 source->address + ':' +
                                 std::to_string(source->port) +
                                 (error.empty() ? "" : ": " + error));
      }
      impl_->description =
          "TLS TCP " + source->address + ':' + std::to_string(source->port);
#else
      throw std::runtime_error("TLS TCP replication is unavailable in this build");
#endif
    } else {
      auto transport = SocketSbeTransport::open(
          SbeSocketEndpoint::tcp(source->address, source->port), 5s, 64 * 1024,
          &error);
      if (!transport) {
        throw std::runtime_error("could not connect to upstream TCP " +
                                 source->address + ':' +
                                 std::to_string(source->port) +
                                 (error.empty() ? "" : ": " + error));
      }
      impl_->socket_client =
          std::make_unique<SocketSbeTransport>(std::move(*transport));
      impl_->description =
          "TCP " + source->address + ':' + std::to_string(source->port);
    }
  } else {
#ifdef GOBLIN_HAS_RDMA
    const auto& source = std::get<ReplicaRdmaConfig>(config);
    std::string error;
    auto client = rdma::RdmaClient::open(source.address, source.port,
                                         source.bytes, 5s, &error);
    if (!client) {
      throw std::runtime_error("could not connect to upstream RDMA " +
                               source.address + ':' +
                               std::to_string(source.port) +
                               (error.empty() ? "" : ": " + error));
    }
    impl_->rdma_client =
        std::make_unique<rdma::RdmaClient>(std::move(*client));
    impl_->description =
        "RDMA " + source.address + ':' + std::to_string(source.port);
#else
    throw std::runtime_error("RDMA replication is unavailable in this build");
#endif
  }

  if (auth != nullptr) {
    const std::string_view fields[]{"AUTH", auth->username, auth->password};
    impl_->send(ring::encode_command(fields));
    const auto response = impl_->read(5s);
    if (response != "+OK\r\n") {
      throw std::runtime_error("upstream firehose authentication failed");
    }
  }

  const std::string_view command[]{"GOBLIN.FIREHOSE"};
  impl_->send(ring::encode_command(command));
  const auto response = impl_->read(5s);
  std::string error;
  auto hello = decode_firehose_hello(response, error);
  if (!hello) {
    throw std::runtime_error("invalid upstream firehose hello: " + error);
  }
  impl_->hello = *hello;
}

ReplicationFollowerRuntime::~ReplicationFollowerRuntime() = default;

const FirehoseHello& ReplicationFollowerRuntime::hello() const noexcept {
  return impl_->hello;
}

const std::string& ReplicationFollowerRuntime::description() const noexcept {
  return impl_->description;
}

bool ReplicationFollowerRuntime::polled() const noexcept {
  return impl_->ring_client != nullptr
#ifdef GOBLIN_HAS_RDMA
         || impl_->rdma_client != nullptr
#endif
      ;
}

int ReplicationFollowerRuntime::notification_fd() const noexcept {
  if (impl_->socket_client) return impl_->socket_client->native_handle();
#ifdef GOBLIN_HAS_TLS
  if (impl_->tls_client) return impl_->tls_client->native_handle();
#endif
  return -1;
}

bool ReplicationFollowerRuntime::has_buffered() const noexcept {
  return !impl_->buffered.empty();
}

bool ReplicationFollowerRuntime::buffer_available(std::string& error) {
  error.clear();
  try {
    while (auto frame = impl_->try_read()) {
      auto batch = decode_firehose_batch(*frame, error);
      if (!batch) return false;
      if (batch->id != impl_->hello.id) {
        error = "upstream firehose changed replication lineage";
        return false;
      }
      if (!impl_->buffered.push(impl_->next_buffer_sequence++, *frame)) {
        error = "replication catch-up buffer exhausted (" +
                std::to_string(impl_->buffered.mapped_bytes()) + " bytes)";
        return false;
      }
    }
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

bool ReplicationFollowerRuntime::try_next(ReplicationBatch& batch,
                                          bool& available,
                                          std::string& error) {
  available = false;
  error.clear();
  try {
    std::optional<std::string> owned;
    std::string_view frame;
    auto buffered = impl_->buffered.front();
    if (buffered) {
      frame = buffered->bytes;
    } else {
      owned = impl_->try_read();
      if (!owned) return true;
      frame = *owned;
    }

    auto decoded = decode_firehose_batch(frame, error);
    if (!decoded) return false;
    if (decoded->id != impl_->hello.id) {
      error = "upstream firehose changed replication lineage";
      return false;
    }
    batch = std::move(*decoded);
    if (buffered) impl_->buffered.pop_front(buffered->bytes.size());
    available = true;
    return true;
  } catch (const std::exception& exception) {
    error = exception.what();
    return false;
  }
}

}  // namespace goblin::core
