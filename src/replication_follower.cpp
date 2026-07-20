#include "replication_follower.hpp"

#include "goblin/core/rdma_client.hpp"
#include "goblin/core/ring_client.hpp"
#include "goblin/core/sbe_socket_transport.hpp"
#include "pubsub.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <thread>

namespace goblin::core {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

}  // namespace

struct ReplicationFollowerRuntime::Impl {
  std::unique_ptr<ring::RingClient> ring_client;
  std::unique_ptr<SocketSbeTransport> socket_client;
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
      auto bytes = socket_client->peek();
      if (!bytes) return std::nullopt;
      socket_pending.append(*bytes);
      socket_client->pop();
    }
  }

  [[nodiscard]] std::optional<std::string> try_read() {
    if (ring_client) return ring_client->try_read_reply();
    if (socket_client) return try_read_socket();
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
      if (socket_client) {
        std::this_thread::sleep_for(50us);
      } else {
        ring::cpu_relax();
      }
    }
  }
};

ReplicationFollowerRuntime::ReplicationFollowerRuntime(
    const ReplicaSourceConfig& config, std::size_t buffer_bytes)
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
  return impl_->socket_client ? impl_->socket_client->native_handle() : -1;
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
