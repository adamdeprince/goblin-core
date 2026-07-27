#include "goblin/core/libfabric_transport.hpp"

#if defined(GOBLIN_HAS_LIBFABRIC)

#include "goblin/core/libfabric_wire.hpp"
#include "goblin/core/ring_buffer.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <rdma/fabric.h>
#include <rdma/fi_cm.h>
#include <rdma/fi_domain.h>
#include <rdma/fi_endpoint.h>
#include <rdma/fi_errno.h>
#include <rdma/fi_eq.h>

namespace goblin::core::fabric {
namespace {

using Clock = std::chrono::steady_clock;
namespace wire = libfabric_wire;

constexpr std::size_t kIoSlots = 16;
constexpr std::size_t kTargetPayloadBytes = 128U * 1024U;
constexpr std::size_t kMaxQueuedInputBytes = 16U * 1024U * 1024U;
constexpr std::array<char, 4> kBootstrapMagic{'G', 'F', 'B', '1'};
constexpr std::size_t kBootstrapHeaderBytes = 8;

[[nodiscard]] std::string fi_error(std::string_view operation,
                                   ssize_t result) {
  return std::string(operation) + ": " +
         fi_strerror(static_cast<int>(-result));
}

[[nodiscard]] std::string errno_error(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

void close_fd(int& fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
    fd = -1;
  }
}

[[nodiscard]] bool set_nonblocking(int fd) noexcept {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void put_u32(std::span<std::byte> bytes, std::size_t offset,
             std::uint32_t value) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  std::memcpy(bytes.data() + offset, &value, sizeof(value));
}

[[nodiscard]] std::uint32_t get_u32(std::span<const std::byte> bytes,
                                    std::size_t offset) noexcept {
  std::uint32_t value = 0;
  std::memcpy(&value, bytes.data() + offset, sizeof(value));
  if constexpr (std::endian::native == std::endian::big) {
    value = std::byteswap(value);
  }
  return value;
}

[[nodiscard]] std::vector<std::byte> bootstrap_record(
    std::span<const std::byte> address) {
  std::vector<std::byte> result(kBootstrapHeaderBytes + address.size());
  std::memcpy(result.data(), kBootstrapMagic.data(), kBootstrapMagic.size());
  put_u32(result, 4, static_cast<std::uint32_t>(address.size()));
  std::memcpy(result.data() + kBootstrapHeaderBytes, address.data(),
              address.size());
  return result;
}

[[nodiscard]] int create_bootstrap_listener(std::string_view address,
                                            std::uint16_t port,
                                            std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_PASSIVE;

  addrinfo* addresses = nullptr;
  const std::string node(address);
  const std::string service = std::to_string(port);
  const int gai = ::getaddrinfo(node.empty() ? nullptr : node.c_str(),
                                service.c_str(), &hints, &addresses);
  if (gai != 0) {
    error = std::string("resolve bootstrap listener: ") +
            ::gai_strerror(gai);
    return -1;
  }

  int result = -1;
  for (auto* current = addresses; current != nullptr;
       current = current->ai_next) {
    result = ::socket(current->ai_family, current->ai_socktype,
                      current->ai_protocol);
    if (result < 0) continue;
    int one = 1;
    (void)::setsockopt(result, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (::bind(result, current->ai_addr,
               static_cast<socklen_t>(current->ai_addrlen)) == 0 &&
        ::listen(result, 128) == 0 && set_nonblocking(result)) {
      break;
    }
    close_fd(result);
  }
  ::freeaddrinfo(addresses);
  if (result < 0) {
    error = errno_error("bind libfabric bootstrap listener");
  }
  return result;
}

[[nodiscard]] bool wait_for_connect(int fd, Clock::time_point deadline,
                                    std::string& error) {
  for (;;) {
    const auto now = Clock::now();
    if (now >= deadline) {
      error = "libfabric bootstrap connect timed out";
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd descriptor{.fd = fd, .events = POLLOUT, .revents = 0};
    const int ready =
        ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) {
      error = ready == 0 ? "libfabric bootstrap connect timed out"
                         : errno_error("poll libfabric bootstrap");
      return false;
    }
    int socket_error = 0;
    socklen_t length = sizeof(socket_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &socket_error, &length) != 0) {
      error = errno_error("read libfabric bootstrap socket status");
      return false;
    }
    if (socket_error != 0) {
      error = "connect libfabric bootstrap: " +
              std::string(std::strerror(socket_error));
      return false;
    }
    return true;
  }
}

[[nodiscard]] int connect_bootstrap(std::string_view host, std::uint16_t port,
                                    Clock::time_point deadline,
                                    std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  addrinfo* addresses = nullptr;
  const std::string node(host);
  const std::string service = std::to_string(port);
  const int gai =
      ::getaddrinfo(node.c_str(), service.c_str(), &hints, &addresses);
  if (gai != 0) {
    error = std::string("resolve libfabric bootstrap: ") +
            ::gai_strerror(gai);
    return -1;
  }

  int result = -1;
  for (auto* current = addresses; current != nullptr;
       current = current->ai_next) {
    result = ::socket(current->ai_family, current->ai_socktype,
                      current->ai_protocol);
    if (result < 0) continue;
    if (!set_nonblocking(result)) {
      close_fd(result);
      continue;
    }
    if (::connect(result, current->ai_addr,
                  static_cast<socklen_t>(current->ai_addrlen)) == 0 ||
        (errno == EINPROGRESS && wait_for_connect(result, deadline, error))) {
      break;
    }
    close_fd(result);
  }
  ::freeaddrinfo(addresses);
  if (result < 0 && error.empty()) {
    error = errno_error("connect libfabric bootstrap");
  }
  return result;
}

[[nodiscard]] bool read_exact(int fd, std::span<std::byte> destination,
                              Clock::time_point deadline, std::string& error) {
  std::size_t offset = 0;
  while (offset < destination.size()) {
    const ssize_t received =
        ::recv(fd, destination.data() + offset, destination.size() - offset, 0);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received == 0) {
      error = "libfabric bootstrap closed before sending its address";
      return false;
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      error = errno_error("read libfabric bootstrap");
      return false;
    }
    const auto now = Clock::now();
    if (now >= deadline) {
      error = "libfabric bootstrap address read timed out";
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
    pollfd descriptor{.fd = fd, .events = POLLIN, .revents = 0};
    const int ready =
        ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (ready < 0 && errno == EINTR) continue;
    if (ready <= 0) {
      error = ready == 0 ? "libfabric bootstrap address read timed out"
                         : errno_error("poll libfabric bootstrap address");
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::optional<std::vector<std::byte>> fetch_server_address(
    std::string_view host, std::uint16_t port, Clock::time_point deadline,
    std::string& error) {
  int fd = connect_bootstrap(host, port, deadline, error);
  if (fd < 0) return std::nullopt;

  std::array<std::byte, kBootstrapHeaderBytes> header{};
  if (!read_exact(fd, header, deadline, error)) {
    close_fd(fd);
    return std::nullopt;
  }
  if (std::memcmp(header.data(), kBootstrapMagic.data(),
                  kBootstrapMagic.size()) != 0) {
    error = "libfabric bootstrap returned an incompatible header";
    close_fd(fd);
    return std::nullopt;
  }
  const std::uint32_t address_bytes = get_u32(header, 4);
  if (address_bytes == 0 ||
      address_bytes > std::numeric_limits<std::uint16_t>::max()) {
    error = "libfabric bootstrap returned an invalid endpoint address";
    close_fd(fd);
    return std::nullopt;
  }
  std::vector<std::byte> address(address_bytes);
  const bool read = read_exact(fd, address, deadline, error);
  close_fd(fd);
  if (!read) return std::nullopt;
  return address;
}

struct OperationContext {
  fi_context context{};
  std::size_t index{0};
};

static_assert(offsetof(OperationContext, context) == 0);

struct ReceiveCompletion {
  std::string bytes;
  fi_addr_t source{FI_ADDR_NOTAVAIL};
};

struct FabricResources {
  fi_info* info{nullptr};
  fid_fabric* fabric{nullptr};
  fid_domain* domain{nullptr};
  fid_av* av{nullptr};
  fid_cq* tx_cq{nullptr};
  fid_cq* rx_cq{nullptr};
  fid_ep* endpoint{nullptr};
  fid_mr* io_mr{nullptr};
  std::vector<std::byte> io_memory;
  std::vector<OperationContext> rx_contexts;
  std::vector<OperationContext> tx_contexts;
  std::array<bool, kIoSlots> tx_busy{};
  std::vector<std::byte> local_address;
  std::size_t frame_bytes{0};
  std::size_t max_payload_bytes{0};
  std::string provider_name;
  SendMode send_mode{SendMode::auto_inject};

  FabricResources() = default;
  FabricResources(const FabricResources&) = delete;
  FabricResources& operator=(const FabricResources&) = delete;

  ~FabricResources() { reset(); }

  void reset() noexcept {
    if (endpoint != nullptr) {
      (void)fi_close(&endpoint->fid);
      endpoint = nullptr;
    }
    if (io_mr != nullptr) {
      (void)fi_close(&io_mr->fid);
      io_mr = nullptr;
    }
    if (rx_cq != nullptr) {
      (void)fi_close(&rx_cq->fid);
      rx_cq = nullptr;
    }
    if (tx_cq != nullptr) {
      (void)fi_close(&tx_cq->fid);
      tx_cq = nullptr;
    }
    if (av != nullptr) {
      (void)fi_close(&av->fid);
      av = nullptr;
    }
    if (domain != nullptr) {
      (void)fi_close(&domain->fid);
      domain = nullptr;
    }
    if (fabric != nullptr) {
      (void)fi_close(&fabric->fid);
      fabric = nullptr;
    }
    if (info != nullptr) {
      fi_freeinfo(info);
      info = nullptr;
    }
  }

  [[nodiscard]] void* descriptor() const noexcept {
    return io_mr == nullptr ? nullptr : fi_mr_desc(io_mr);
  }

  [[nodiscard]] std::byte* rx_buffer(std::size_t index) noexcept {
    return io_memory.data() + index * frame_bytes;
  }

  [[nodiscard]] std::byte* tx_buffer(std::size_t index) noexcept {
    return io_memory.data() + (kIoSlots + index) * frame_bytes;
  }

  [[nodiscard]] bool post_receive(std::size_t index, std::string& error) {
    const ssize_t result =
        fi_recv(endpoint, rx_buffer(index), frame_bytes, descriptor(),
                FI_ADDR_UNSPEC, &rx_contexts[index].context);
    if (result != 0) {
      error = fi_error("post libfabric receive", result);
      return false;
    }
    return true;
  }

  [[nodiscard]] bool initialize(std::string_view provider,
                                std::string_view source_address,
                                SendMode requested_send_mode,
                                std::string& error) {
    send_mode = requested_send_mode;
    fi_info* hints = fi_allocinfo();
    if (hints == nullptr) {
      error = "allocate libfabric hints: out of memory";
      return false;
    }
    hints->caps = FI_MSG | FI_SOURCE;
    hints->mode = FI_CONTEXT;
    hints->ep_attr->type = FI_EP_RDM;
    hints->domain_attr->threading = FI_THREAD_SAFE;
    hints->domain_attr->resource_mgmt = FI_RM_ENABLED;
    hints->domain_attr->data_progress = FI_PROGRESS_MANUAL;
    hints->domain_attr->control_progress = FI_PROGRESS_MANUAL;
    hints->tx_attr->op_flags = FI_COMPLETION;
    hints->rx_attr->op_flags = FI_COMPLETION;
    hints->tx_attr->msg_order = FI_ORDER_SAS;
    hints->rx_attr->msg_order = FI_ORDER_SAS;
    hints->fabric_attr->prov_name = ::strdup(std::string(provider).c_str());
    if (hints->fabric_attr->prov_name == nullptr) {
      fi_freeinfo(hints);
      error = "copy libfabric provider name: out of memory";
      return false;
    }

    const std::string source(source_address);
    const std::uint64_t getinfo_flags =
        source.empty() ? 0 : static_cast<std::uint64_t>(FI_SOURCE);
    ssize_t result =
        fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
                   source.empty() ? nullptr : source.c_str(), nullptr,
                   getinfo_flags, hints, &info);
    if (result != 0 && provider == "efa" && !source.empty()) {
      result = fi_getinfo(FI_VERSION(FI_MAJOR_VERSION, FI_MINOR_VERSION),
                          nullptr, nullptr, 0, hints, &info);
    }
    fi_freeinfo(hints);
    if (result != 0) {
      error = fi_error("select libfabric provider", result);
      return false;
    }
    provider_name =
        info->fabric_attr->prov_name == nullptr ? std::string(provider)
                                                : info->fabric_attr->prov_name;

    result = fi_fabric(info->fabric_attr, &fabric, nullptr);
    if (result != 0) {
      error = fi_error("open libfabric fabric", result);
      return false;
    }
    result = fi_domain(fabric, info, &domain, nullptr);
    if (result != 0) {
      error = fi_error("open libfabric domain", result);
      return false;
    }

    fi_av_attr av_attributes{};
    av_attributes.type = FI_AV_TABLE;
    av_attributes.count = 65536;
    result = fi_av_open(domain, &av_attributes, &av, nullptr);
    if (result != 0) {
      error = fi_error("open libfabric address vector", result);
      return false;
    }

    fi_cq_attr cq_attributes{};
    cq_attributes.format = FI_CQ_FORMAT_MSG;
    cq_attributes.size = 4 * kIoSlots;
    cq_attributes.wait_obj = FI_WAIT_NONE;
    result = fi_cq_open(domain, &cq_attributes, &tx_cq, nullptr);
    if (result != 0) {
      error = fi_error("open libfabric transmit CQ", result);
      return false;
    }
    result = fi_cq_open(domain, &cq_attributes, &rx_cq, nullptr);
    if (result != 0) {
      error = fi_error("open libfabric receive CQ", result);
      return false;
    }

    result = fi_endpoint(domain, info, &endpoint, nullptr);
    if (result != 0) {
      error = fi_error("open libfabric RDM endpoint", result);
      return false;
    }
    result = fi_ep_bind(endpoint, &av->fid, 0);
    if (result == 0) {
      result = fi_ep_bind(endpoint, &tx_cq->fid, FI_TRANSMIT);
    }
    if (result == 0) {
      result = fi_ep_bind(endpoint, &rx_cq->fid, FI_RECV);
    }
    if (result == 0) result = fi_enable(endpoint);
    if (result != 0) {
      error = fi_error("bind/enable libfabric RDM endpoint", result);
      return false;
    }

    if (info->ep_attr->max_msg_size <= wire::kHeaderBytes) {
      error = "libfabric provider's maximum message is too small";
      return false;
    }
    max_payload_bytes =
        std::min(kTargetPayloadBytes,
                 info->ep_attr->max_msg_size - wire::kHeaderBytes);
    frame_bytes = wire::kHeaderBytes + max_payload_bytes;
    if (frame_bytes >
        std::numeric_limits<std::size_t>::max() / (2 * kIoSlots)) {
      error = "libfabric I/O pool geometry overflow";
      return false;
    }
    io_memory.resize(2 * kIoSlots * frame_bytes);
    std::fill(io_memory.begin(), io_memory.end(), std::byte{0});

    if ((info->domain_attr->mr_mode & FI_MR_LOCAL) != 0) {
      result = fi_mr_reg(domain, io_memory.data(), io_memory.size(),
                         FI_SEND | FI_RECV, 0, 0, 0, &io_mr, nullptr);
      if (result != 0) {
        error = fi_error("register libfabric I/O pool", result);
        return false;
      }
    }

    rx_contexts.resize(kIoSlots);
    tx_contexts.resize(kIoSlots);
    for (std::size_t index = 0; index < kIoSlots; ++index) {
      rx_contexts[index].index = index;
      tx_contexts[index].index = index;
      if (!post_receive(index, error)) return false;
    }

    std::size_t address_bytes = 0;
    result = fi_getname(&endpoint->fid, nullptr, &address_bytes);
    if (result != -FI_ETOOSMALL || address_bytes == 0 ||
        address_bytes > std::numeric_limits<std::uint16_t>::max()) {
      error = result == 0
                  ? "libfabric returned an empty endpoint address"
                  : fi_error("query libfabric endpoint address size", result);
      return false;
    }
    local_address.resize(address_bytes);
    result =
        fi_getname(&endpoint->fid, local_address.data(), &address_bytes);
    if (result != 0) {
      error = fi_error("query libfabric endpoint address", result);
      return false;
    }
    local_address.resize(address_bytes);
    return true;
  }

  [[nodiscard]] std::optional<fi_addr_t> insert_address(
      std::span<const std::byte> address, std::string& error) {
    fi_addr_t result = FI_ADDR_UNSPEC;
    const int inserted =
        fi_av_insert(av, address.data(), 1, &result, 0, nullptr);
    if (inserted != 1) {
      error = inserted < 0
                  ? fi_error("insert libfabric peer address", inserted)
                  : "libfabric did not insert the peer address";
      return std::nullopt;
    }
    return result;
  }

  void remove_address(fi_addr_t address) noexcept {
    if (address != FI_ADDR_UNSPEC && address != FI_ADDR_NOTAVAIL) {
      (void)fi_av_remove(av, &address, 1, 0);
    }
  }

  [[nodiscard]] bool progress_tx(bool& progressed, std::string& error) {
    std::array<fi_cq_msg_entry, kIoSlots> entries{};
    const ssize_t count = fi_cq_read(tx_cq, entries.data(), entries.size());
    if (count == -FI_EAGAIN) return true;
    if (count == -FI_EAVAIL) {
      fi_cq_err_entry completion_error{};
      const ssize_t read = fi_cq_readerr(tx_cq, &completion_error, 0);
      error = read > 0
                  ? "libfabric transmit completion failed: " +
                        std::string(fi_strerror(completion_error.err))
                  : fi_error("read libfabric transmit CQ error", read);
      return false;
    }
    if (count < 0) {
      error = fi_error("read libfabric transmit CQ", count);
      return false;
    }
    progressed = progressed || count > 0;
    for (ssize_t index = 0; index < count; ++index) {
      auto* context =
          static_cast<OperationContext*>(entries[index].op_context);
      if (context == nullptr || context->index >= kIoSlots) {
        error = "libfabric transmit CQ returned an invalid operation context";
        return false;
      }
      tx_busy[context->index] = false;
    }
    return true;
  }

  [[nodiscard]] bool receive_one(std::optional<ReceiveCompletion>& completion,
                                 bool& progressed, std::string& error) {
    fi_cq_msg_entry entry{};
    fi_addr_t source = FI_ADDR_NOTAVAIL;
    const ssize_t count = fi_cq_readfrom(rx_cq, &entry, 1, &source);
    if (count == -FI_EAGAIN) return true;
    if (count == -FI_EAVAIL) {
      fi_cq_err_entry completion_error{};
      const ssize_t read = fi_cq_readerr(rx_cq, &completion_error, 0);
      error = read > 0
                  ? "libfabric receive completion failed: " +
                        std::string(fi_strerror(completion_error.err))
                  : fi_error("read libfabric receive CQ error", read);
      return false;
    }
    if (count < 0) {
      error = fi_error("read libfabric receive CQ", count);
      return false;
    }
    if (count == 0) return true;

    progressed = true;
    auto* context = static_cast<OperationContext*>(entry.op_context);
    if (context == nullptr || context->index >= kIoSlots ||
        entry.len > frame_bytes) {
      error = "libfabric receive CQ returned an invalid completion";
      return false;
    }
    ReceiveCompletion result{
        .bytes = std::string(
            reinterpret_cast<const char*>(rx_buffer(context->index)),
            entry.len),
        .source = source};
    if (!post_receive(context->index, error)) return false;
    completion = std::move(result);
    return true;
  }

  [[nodiscard]] bool post_frame(fi_addr_t destination,
                                const wire::Header& header,
                                std::string_view payload,
                                std::string& error) {
    if (payload.size() > max_payload_bytes) return false;
    const std::size_t bytes = wire::kHeaderBytes + payload.size();

    std::size_t slot = kIoSlots;
    for (std::size_t index = 0; index < kIoSlots; ++index) {
      if (!tx_busy[index]) {
        slot = index;
        break;
      }
    }
    if (slot == kIoSlots) return false;

    auto destination_bytes =
        std::span<std::byte>(tx_buffer(slot), frame_bytes);
    wire::Header encoded = header;
    encoded.payload_bytes = static_cast<std::uint32_t>(payload.size());
    if (!wire::encode_header(destination_bytes.first(wire::kHeaderBytes),
                             encoded)) {
      error = "encode libfabric frame header failed";
      return false;
    }
    std::memcpy(destination_bytes.data() + wire::kHeaderBytes, payload.data(),
                payload.size());

    const bool inject =
        send_mode == SendMode::auto_inject &&
        bytes <= info->tx_attr->inject_size;
    ssize_t result = 0;
    if (inject) {
      result = fi_inject(endpoint, destination_bytes.data(), bytes, destination);
    } else {
      result = fi_send(endpoint, destination_bytes.data(), bytes, descriptor(),
                       destination, &tx_contexts[slot].context);
    }
    if (result == -FI_EAGAIN) return false;
    if (result != 0) {
      error = fi_error("send libfabric frame", result);
      return false;
    }
    if (!inject) tx_busy[slot] = true;
    return true;
  }
};

struct IncomingFrame {
  wire::Kind kind{wire::Kind::data};
  std::string payload;
  std::uint64_t reply_to{0};
};

struct InputRecord {
  std::uint64_t sequence{0};
  std::string bytes;
};

struct PendingControl {
  wire::Kind kind{wire::Kind::pong};
  std::uint64_t sequence{std::numeric_limits<std::uint64_t>::max()};
  std::uint64_t reply_to{0};
  std::string payload;
};

[[nodiscard]] std::uint64_t random_session_id() {
  std::random_device random;
  std::uint64_t result =
      (static_cast<std::uint64_t>(random()) << 32) ^
      static_cast<std::uint64_t>(random());
  if (result == 0) result = 1;
  return result;
}

}  // namespace

namespace detail {

struct PeerState {
  PeerState(std::uint64_t assigned_session_id, fi_addr_t assigned_address,
            std::size_t reorder_messages, std::size_t reorder_bytes)
      : session_id(assigned_session_id),
        address(assigned_address),
        inbox(reorder_messages, reorder_bytes) {}

  std::uint64_t session_id{0};
  fi_addr_t address{FI_ADDR_UNSPEC};
  wire::OrderedInbox<IncomingFrame> inbox;
  std::deque<InputRecord> input;
  std::deque<PendingControl> controls;
  std::size_t input_bytes{0};
  std::uint64_t next_reply_sequence{wire::kFirstSequence};
  Clock::time_point last_activity{Clock::now()};
  bool disconnected{false};
  bool failed{false};
  std::string error;
};

struct BootstrapClient {
  int fd{-1};
  std::size_t offset{0};
};

struct ServerState : std::enable_shared_from_this<ServerState> {
  FabricResources fabric;
  int bootstrap_fd{-1};
  std::vector<std::byte> bootstrap_bytes;
  std::vector<BootstrapClient> bootstrap_clients;
  std::unordered_map<std::uint64_t, std::shared_ptr<PeerState>> peers;
  std::vector<std::weak_ptr<PeerState>> peer_cycle;
  std::deque<std::shared_ptr<PeerState>> new_peers;
  std::size_t peer_cursor{0};
  std::uint32_t heartbeat_timeout_ms{kDefaultHeartbeatTimeoutMs};
  std::size_t reorder_messages{kDefaultReorderMessages};
  std::size_t reorder_bytes{kDefaultReorderBytes};
  std::string error_text;

  ~ServerState() {
    for (auto& client : bootstrap_clients) close_fd(client.fd);
    close_fd(bootstrap_fd);
  }

  void fail_peer(PeerState& peer, std::string message) noexcept {
    if (!peer.failed) peer.error = std::move(message);
    peer.failed = true;
    peer.disconnected = true;
  }

  [[nodiscard]] bool flush_control(PeerState& peer) {
    if (peer.controls.empty()) return true;
    auto& control = peer.controls.front();
    const std::uint64_t sequence =
        control.sequence == std::numeric_limits<std::uint64_t>::max()
            ? peer.next_reply_sequence
            : control.sequence;
    const wire::Header header{.kind = control.kind,
                              .session_id = peer.session_id,
                              .sequence = sequence,
                              .reply_to = control.reply_to};
    if (!fabric.post_frame(peer.address, header, control.payload, error_text)) {
      if (!error_text.empty()) fail_peer(peer, error_text);
      return false;
    }
    if (control.sequence == std::numeric_limits<std::uint64_t>::max()) {
      ++peer.next_reply_sequence;
    }
    peer.controls.pop_front();
    return true;
  }

  void queue_control(PeerState& peer, wire::Kind kind, std::uint64_t reply_to,
                     std::string payload = {},
                     std::uint64_t sequence =
                         std::numeric_limits<std::uint64_t>::max()) {
    peer.controls.push_back(PendingControl{.kind = kind,
                                           .sequence = sequence,
                                           .reply_to = reply_to,
                                           .payload = std::move(payload)});
    (void)flush_control(peer);
  }

  void progress_bootstrap(bool& progressed) {
    pollfd listener{.fd = bootstrap_fd, .events = POLLIN, .revents = 0};
    const int ready = ::poll(&listener, 1, 0);
    if (ready < 0) {
      if (errno != EINTR) {
        error_text = errno_error("poll libfabric bootstrap listener");
      }
      return;
    }
    if (ready > 0 && (listener.revents & POLLNVAL) != 0) {
      error_text = "libfabric bootstrap listener became invalid";
      return;
    }
    if (ready > 0 && (listener.revents & POLLIN) != 0) {
      for (;;) {
        const int accepted = ::accept(bootstrap_fd, nullptr, nullptr);
        if (accepted < 0) {
          if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            error_text = errno_error("accept libfabric bootstrap");
          }
          break;
        }
        if (!set_nonblocking(accepted)) {
          int temporary = accepted;
          close_fd(temporary);
          continue;
        }
        bootstrap_clients.push_back(BootstrapClient{.fd = accepted});
        progressed = true;
      }
    }

    for (std::size_t count = bootstrap_clients.size(); count > 0; --count) {
      const std::size_t index = count - 1;
      auto& client = bootstrap_clients[index];
      const auto remaining =
          std::span<const std::byte>(bootstrap_bytes).subspan(client.offset);
      const ssize_t sent =
          ::send(client.fd, remaining.data(), remaining.size(), MSG_NOSIGNAL);
      if (sent > 0) {
        client.offset += static_cast<std::size_t>(sent);
        progressed = true;
      } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                 errno != EINTR) {
        client.offset = bootstrap_bytes.size();
      }
      if (client.offset == bootstrap_bytes.size()) {
        close_fd(client.fd);
        bootstrap_clients.erase(
            bootstrap_clients.begin() + static_cast<std::ptrdiff_t>(index));
      }
    }
  }

  void handle_hello(const wire::Header& header,
                    std::span<const std::byte> payload) {
    if (header.sequence != 0 || header.session_id == 0) return;
    const auto hello = wire::decode_hello(payload);
    if (!hello || hello->goblin_version != GOBLIN_CORE_VERSION ||
        hello->endpoint_address.empty()) {
      return;
    }

    if (auto existing = peers.find(header.session_id);
        existing != peers.end()) {
      existing->second->last_activity = Clock::now();
      return;
    }
    auto address = fabric.insert_address(hello->endpoint_address, error_text);
    if (!address) return;

    const std::size_t requested =
        hello->reorder_messages == 0
            ? reorder_messages
            : std::min<std::size_t>(hello->reorder_messages, reorder_messages);
    auto peer = std::make_shared<PeerState>(
        header.session_id, *address, std::max<std::size_t>(requested, 1),
        reorder_bytes);
    peers.emplace(peer->session_id, peer);
    peer_cycle.emplace_back(peer);

    wire::HelloAck ack{
        .heartbeat_timeout_ms = heartbeat_timeout_ms,
        .reorder_messages = static_cast<std::uint32_t>(
            std::min<std::size_t>(reorder_messages,
                                  std::numeric_limits<std::uint32_t>::max())),
        .max_payload_bytes =
            static_cast<std::uint32_t>(fabric.max_payload_bytes)};
    std::string encoded(wire::kHelloAckBytes, '\0');
    (void)wire::encode_hello_ack(
        std::span<std::byte>(reinterpret_cast<std::byte*>(encoded.data()),
                             encoded.size()),
        ack);
    queue_control(*peer, wire::Kind::hello_ack, 0, std::move(encoded), 0);
    new_peers.push_back(std::move(peer));
  }

  void deliver(PeerState& peer, IncomingFrame frame,
               std::uint64_t sequence) {
    peer.last_activity = Clock::now();
    switch (frame.kind) {
      case wire::Kind::data:
        if (frame.payload.size() > kMaxQueuedInputBytes - peer.input_bytes) {
          fail_peer(peer, "libfabric client input queue exceeded its limit");
          return;
        }
        peer.input_bytes += frame.payload.size();
        peer.input.push_back(
            InputRecord{.sequence = sequence, .bytes = std::move(frame.payload)});
        return;
      case wire::Kind::ping:
        queue_control(peer, wire::Kind::pong, sequence);
        return;
      case wire::Kind::goodbye:
        peer.disconnected = true;
        return;
      case wire::Kind::error:
        fail_peer(peer, frame.payload.empty()
                            ? "libfabric peer reported an error"
                            : std::move(frame.payload));
        return;
      case wire::Kind::hello:
      case wire::Kind::hello_ack:
      case wire::Kind::pong:
        fail_peer(peer, "libfabric client sent an invalid ordered frame kind");
        return;
    }
  }

  void handle_receive(ReceiveCompletion completion) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(completion.bytes.data()),
        completion.bytes.size());
    const auto header = wire::decode_header(bytes);
    if (!header) return;
    const auto payload = bytes.subspan(wire::kHeaderBytes);
    if (header->kind == wire::Kind::hello) {
      handle_hello(*header, payload);
      return;
    }

    auto found = peers.find(header->session_id);
    if (found == peers.end()) return;
    auto& peer = *found->second;
    if (peer.disconnected || peer.failed || header->sequence == 0) return;
    if (completion.source != FI_ADDR_NOTAVAIL &&
        completion.source != peer.address) {
      fail_peer(peer, "libfabric session address changed unexpectedly");
      return;
    }

    IncomingFrame frame{.kind = header->kind,
                        .payload = std::string(
                            reinterpret_cast<const char*>(payload.data()),
                            payload.size()),
                        .reply_to = header->reply_to};
    const auto result = peer.inbox.push(
        header->sequence, std::move(frame), payload.size(),
        [this, &peer](IncomingFrame ready, std::uint64_t sequence) {
          deliver(peer, std::move(ready), sequence);
        });
    if (result == wire::OrderResult::overflow) {
      fail_peer(peer, "libfabric request reorder window overflow");
    }
  }

  void service_one_peer(bool& progressed) {
    if (peer_cycle.empty()) return;
    for (std::size_t tries = 0; tries < peer_cycle.size(); ++tries) {
      peer_cursor %= peer_cycle.size();
      auto peer = peer_cycle[peer_cursor++].lock();
      if (!peer) continue;
      if (!peer->controls.empty()) {
        const std::size_t before = peer->controls.size();
        (void)flush_control(*peer);
        progressed = progressed || peer->controls.size() != before;
      }
      if (heartbeat_timeout_ms != 0 && !peer->disconnected &&
          Clock::now() - peer->last_activity >=
              std::chrono::milliseconds(heartbeat_timeout_ms)) {
        fail_peer(*peer, "libfabric client heartbeat expired");
        progressed = true;
      }
      break;
    }
    if (peer_cycle.size() > peers.size() * 2 + 32) {
      std::erase_if(peer_cycle,
                    [](const auto& peer) { return peer.expired(); });
      peer_cursor = 0;
    }
  }

  [[nodiscard]] ListenerPoll poll() noexcept {
    ListenerPoll result{};
    if (!error_text.empty()) return result;
    try {
      progress_bootstrap(result.progressed);
      if (!error_text.empty()) return result;
      if (!fabric.progress_tx(result.progressed, error_text)) return result;

      std::optional<ReceiveCompletion> completion;
      if (!fabric.receive_one(completion, result.progressed, error_text)) {
        return result;
      }
      if (completion) handle_receive(std::move(*completion));
      service_one_peer(result.progressed);
      if (!new_peers.empty()) {
        auto peer = std::move(new_peers.front());
        new_peers.pop_front();
        result.connection =
            std::unique_ptr<Connection>(new Connection(shared_from_this(),
                                                       std::move(peer)));
        result.progressed = true;
      }
    } catch (const std::exception& exception) {
      error_text = exception.what();
    } catch (...) {
      error_text = "unknown libfabric server transport failure";
    }
    return result;
  }

  void detach(const std::shared_ptr<PeerState>& peer) noexcept {
    if (!peer) return;
    auto found = peers.find(peer->session_id);
    if (found != peers.end() && found->second == peer) peers.erase(found);
    fabric.remove_address(peer->address);
    peer->disconnected = true;
  }
};

struct ClientState {
  FabricResources fabric;
  fi_addr_t server_address{FI_ADDR_UNSPEC};
  std::uint64_t session_id{0};
  std::uint64_t next_request_sequence{wire::kFirstSequence};
  wire::OrderedInbox<IncomingFrame> inbox{kDefaultReorderMessages,
                                           kDefaultReorderBytes};
  std::deque<std::string> records;
  std::size_t record_bytes{0};
  std::mutex mutex;
  std::mutex heartbeat_wait_mutex;
  std::condition_variable_any heartbeat_wait;
  std::jthread heartbeat_thread;
  Clock::time_point last_send{Clock::now()};
  std::uint32_t heartbeat_timeout_ms{0};
  std::size_t negotiated_max_payload{0};
  bool hello_acknowledged{false};
  bool failed_state{false};
  bool shutdown_started{false};
  std::string error_text;

  ~ClientState() { shutdown(); }

  void fail(std::string message) noexcept {
    if (!failed_state) error_text = std::move(message);
    failed_state = true;
  }

  [[nodiscard]] bool post(wire::Kind kind, std::uint64_t sequence,
                          std::uint64_t reply_to, std::string_view payload) {
    const wire::Header header{.kind = kind,
                              .session_id = session_id,
                              .sequence = sequence,
                              .reply_to = reply_to};
    const bool sent =
        fabric.post_frame(server_address, header, payload, error_text);
    if (!sent && !error_text.empty()) fail(error_text);
    return sent;
  }

  void deliver(IncomingFrame frame, std::uint64_t) {
    switch (frame.kind) {
      case wire::Kind::data:
        if (frame.payload.size() > kMaxQueuedInputBytes - record_bytes) {
          fail("libfabric client reply queue exceeded its limit");
          return;
        }
        record_bytes += frame.payload.size();
        records.push_back(std::move(frame.payload));
        return;
      case wire::Kind::pong:
        return;
      case wire::Kind::goodbye:
        fail("libfabric server closed the session");
        return;
      case wire::Kind::error:
        fail(frame.payload.empty() ? "libfabric server reported an error"
                                   : std::move(frame.payload));
        return;
      case wire::Kind::hello:
      case wire::Kind::hello_ack:
      case wire::Kind::ping:
        fail("libfabric server sent an invalid ordered frame kind");
        return;
    }
  }

  void handle_receive(ReceiveCompletion completion) {
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(completion.bytes.data()),
        completion.bytes.size());
    const auto header = wire::decode_header(bytes);
    if (!header || header->session_id != session_id) return;
    const auto payload = bytes.subspan(wire::kHeaderBytes);

    if (header->kind == wire::Kind::hello_ack) {
      if (header->sequence != 0 || hello_acknowledged) return;
      const auto ack = wire::decode_hello_ack(payload);
      if (!ack || ack->max_payload_bytes == 0) {
        fail("libfabric server returned an invalid hello acknowledgement");
        return;
      }
      heartbeat_timeout_ms = ack->heartbeat_timeout_ms;
      negotiated_max_payload =
          std::min<std::size_t>(ack->max_payload_bytes,
                                fabric.max_payload_bytes);
      inbox = wire::OrderedInbox<IncomingFrame>(
          std::max<std::size_t>(ack->reorder_messages, 1),
          kDefaultReorderBytes);
      hello_acknowledged = true;
      return;
    }
    if (!hello_acknowledged || header->sequence == 0) return;

    IncomingFrame frame{.kind = header->kind,
                        .payload = std::string(
                            reinterpret_cast<const char*>(payload.data()),
                            payload.size()),
                        .reply_to = header->reply_to};
    const auto ordered = inbox.push(
        header->sequence, std::move(frame), payload.size(),
        [this](IncomingFrame ready, std::uint64_t sequence) {
          deliver(std::move(ready), sequence);
        });
    if (ordered == wire::OrderResult::overflow) {
      fail("libfabric reply reorder window overflow");
    }
  }

  void progress_locked() {
    if (failed_state) return;
    bool progressed = false;
    if (!fabric.progress_tx(progressed, error_text)) {
      fail(error_text);
      return;
    }
    std::optional<ReceiveCompletion> completion;
    if (!fabric.receive_one(completion, progressed, error_text)) {
      fail(error_text);
      return;
    }
    if (completion) handle_receive(std::move(*completion));
  }

  [[nodiscard]] bool send_data(std::string_view bytes) {
    std::lock_guard lock(mutex);
    if (failed_state || !hello_acknowledged ||
        bytes.size() > negotiated_max_payload) {
      if (!failed_state && bytes.size() > negotiated_max_payload) {
        fail("libfabric request exceeds the negotiated message limit");
      }
      return false;
    }
    progress_locked();
    if (failed_state) return false;
    if (!post(wire::Kind::data, next_request_sequence, 0, bytes)) return false;
    ++next_request_sequence;
    last_send = Clock::now();
    return true;
  }

  void start_heartbeat() {
    if (heartbeat_timeout_ms == 0) return;
    heartbeat_thread = std::jthread([this](std::stop_token stop) {
      const auto interval = std::max(
          std::chrono::milliseconds(1),
          std::chrono::milliseconds(heartbeat_timeout_ms) / 3);
      std::unique_lock wait_lock(heartbeat_wait_mutex);
      while (!stop.stop_requested()) {
        heartbeat_wait.wait_for(wait_lock, stop, interval,
                                [] { return false; });
        if (stop.stop_requested()) break;
        std::lock_guard lock(mutex);
        if (failed_state || shutdown_started) break;
        progress_locked();
        if (failed_state || Clock::now() - last_send < interval) continue;
        if (post(wire::Kind::ping, next_request_sequence,
                 0, {})) {
          ++next_request_sequence;
          last_send = Clock::now();
        }
      }
    });
  }

  void shutdown() noexcept {
    {
      std::lock_guard lock(mutex);
      if (shutdown_started) return;
      shutdown_started = true;
      if (!failed_state && hello_acknowledged) {
        (void)post(wire::Kind::goodbye, next_request_sequence, 0, {});
      }
    }
    if (heartbeat_thread.joinable()) {
      heartbeat_thread.request_stop();
      heartbeat_wait.notify_all();
      heartbeat_thread.join();
    }
    fabric.remove_address(server_address);
  }
};

}  // namespace detail

Connection::Connection(std::shared_ptr<detail::ServerState> server,
                       std::shared_ptr<detail::PeerState> peer) noexcept
    : server_(std::move(server)), peer_(std::move(peer)) {}

Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;

Connection::~Connection() {
  if (server_ && peer_) server_->detach(peer_);
}

bool Connection::established() const noexcept {
  return peer_ && !peer_->disconnected && !peer_->failed;
}

bool Connection::disconnected() const noexcept {
  return !peer_ || peer_->disconnected;
}

bool Connection::failed() const noexcept {
  return !peer_ || peer_->failed;
}

std::string_view Connection::error() const noexcept {
  return peer_ ? std::string_view(peer_->error) : std::string_view{};
}

std::optional<std::string_view> Connection::peek() noexcept {
  if (!peer_ || peer_->input.empty()) return std::nullopt;
  return peer_->input.front().bytes;
}

void Connection::pop() noexcept {
  if (!peer_ || peer_->input.empty()) return;
  peer_->input_bytes -= peer_->input.front().bytes.size();
  peer_->input.pop_front();
}

void Connection::wait_for_record() const noexcept { ring::cpu_relax(); }

std::uint64_t Connection::current_sequence() const noexcept {
  return !peer_ || peer_->input.empty() ? 0 : peer_->input.front().sequence;
}

void Connection::set_reply_to(std::uint64_t request_sequence) noexcept {
  reply_to_ = request_sequence;
}

bool Connection::try_push(std::string_view bytes) noexcept {
  if (!server_ || !peer_ || !established() ||
      bytes.size() > server_->fabric.max_payload_bytes) {
    return false;
  }
  if (!peer_->controls.empty() && !server_->flush_control(*peer_)) return false;
  const wire::Header header{.kind = wire::Kind::data,
                            .session_id = peer_->session_id,
                            .sequence = peer_->next_reply_sequence,
                            .reply_to = reply_to_};
  if (!server_->fabric.post_frame(peer_->address, header, bytes,
                                  server_->error_text)) {
    if (!server_->error_text.empty()) {
      server_->fail_peer(*peer_, server_->error_text);
    }
    return false;
  }
  ++peer_->next_reply_sequence;
  return true;
}

std::size_t Connection::max_record_payload() const noexcept {
  return server_ ? server_->fabric.max_payload_bytes : 0;
}

void Connection::disconnect() noexcept {
  if (!server_ || !peer_ || peer_->disconnected) return;
  server_->queue_control(*peer_, wire::Kind::goodbye, 0);
  peer_->disconnected = true;
}

ServerListener::ServerListener(
    std::shared_ptr<detail::ServerState> state) noexcept
    : state_(std::move(state)) {}

ServerListener::ServerListener(ServerListener&&) noexcept = default;
ServerListener& ServerListener::operator=(ServerListener&&) noexcept = default;
ServerListener::~ServerListener() = default;

std::unique_ptr<ServerListener> ServerListener::create(
    std::string_view provider, std::string_view bootstrap_address,
    std::uint16_t bootstrap_port, std::uint32_t heartbeat_timeout_ms,
    std::size_t reorder_messages, std::size_t reorder_bytes,
    std::string& error, SendMode send_mode) {
  if (provider.empty() || bootstrap_address.empty() ||
      bootstrap_port == 0 || reorder_messages == 0 || reorder_bytes == 0) {
    error = "invalid libfabric listener configuration";
    return nullptr;
  }
  auto state = std::make_shared<detail::ServerState>();
  state->heartbeat_timeout_ms = heartbeat_timeout_ms;
  state->reorder_messages = reorder_messages;
  state->reorder_bytes = reorder_bytes;
  if (!state->fabric.initialize(provider, bootstrap_address, send_mode,
                                error)) {
    return nullptr;
  }
  state->bootstrap_bytes = bootstrap_record(state->fabric.local_address);
  state->bootstrap_fd =
      create_bootstrap_listener(bootstrap_address, bootstrap_port, error);
  if (state->bootstrap_fd < 0) return nullptr;
  return std::unique_ptr<ServerListener>(
      new ServerListener(std::move(state)));
}

ListenerPoll ServerListener::poll() noexcept {
  return state_ ? state_->poll() : ListenerPoll{};
}

std::string_view ServerListener::error() const noexcept {
  return state_ ? std::string_view(state_->error_text) : std::string_view{};
}

std::string_view ServerListener::provider() const noexcept {
  return state_ ? std::string_view(state_->fabric.provider_name)
                : std::string_view{};
}

ClientTransport::ClientTransport(
    std::shared_ptr<detail::ClientState> state,
    std::size_t buffer_size) noexcept
    : state_(std::move(state)), buffer_size_(buffer_size) {}

ClientTransport::ClientTransport(ClientTransport&&) noexcept = default;
ClientTransport& ClientTransport::operator=(ClientTransport&&) noexcept =
    default;

ClientTransport::~ClientTransport() {
  if (state_) state_->shutdown();
}

std::optional<ClientTransport> ClientTransport::open(
    std::string_view provider, std::string_view host,
    std::uint16_t bootstrap_port, ms timeout, std::size_t buffer_size,
    std::string_view local_address, std::string* error,
    SendMode send_mode) {
  const auto deadline = Clock::now() + timeout;
  std::string local_error;
  auto state = std::make_shared<detail::ClientState>();
  if (!state->fabric.initialize(provider, local_address, send_mode,
                                local_error)) {
    if (error) *error = std::move(local_error);
    return std::nullopt;
  }
  auto server =
      fetch_server_address(host, bootstrap_port, deadline, local_error);
  if (!server) {
    if (error) *error = std::move(local_error);
    return std::nullopt;
  }
  auto address = state->fabric.insert_address(*server, local_error);
  if (!address) {
    if (error) *error = std::move(local_error);
    return std::nullopt;
  }
  state->server_address = *address;
  state->session_id = random_session_id();

  const std::size_t hello_bytes = wire::hello_payload_bytes(
      GOBLIN_CORE_VERSION, state->fabric.local_address.size());
  std::string hello(hello_bytes, '\0');
  (void)wire::encode_hello(
      std::span<std::byte>(reinterpret_cast<std::byte*>(hello.data()),
                           hello.size()),
      GOBLIN_CORE_VERSION, state->fabric.local_address,
      static_cast<std::uint32_t>(kDefaultReorderMessages));

  while (!state->post(wire::Kind::hello, 0, 0, hello)) {
    if (!state->error_text.empty() || Clock::now() >= deadline) {
      if (error) {
        *error = state->error_text.empty()
                     ? "libfabric hello send timed out"
                     : state->error_text;
      }
      return std::nullopt;
    }
    std::lock_guard lock(state->mutex);
    state->progress_locked();
    ring::cpu_relax();
  }

  while (!state->hello_acknowledged && !state->failed_state &&
         Clock::now() < deadline) {
    {
      std::lock_guard lock(state->mutex);
      state->progress_locked();
    }
    ring::cpu_relax();
  }
  if (!state->hello_acknowledged || state->failed_state) {
    if (error) {
      *error = state->error_text.empty()
                   ? "libfabric hello acknowledgement timed out"
                   : state->error_text;
    }
    return std::nullopt;
  }
  state->last_send = Clock::now();
  state->start_heartbeat();
  return ClientTransport(std::move(state), buffer_size);
}

bool ClientTransport::send_one(std::string_view bytes) noexcept {
  return state_ && state_->send_data(bytes);
}

std::optional<std::string_view> ClientTransport::peek() noexcept {
  if (!state_) return std::nullopt;
  if (current_record_) return *current_record_;
  std::lock_guard lock(state_->mutex);
  state_->progress_locked();
  if (state_->records.empty()) return std::nullopt;
  current_record_ = std::move(state_->records.front());
  state_->record_bytes -= current_record_->size();
  state_->records.pop_front();
  return *current_record_;
}

void ClientTransport::pop() noexcept { current_record_.reset(); }

void ClientTransport::wait_for_record() noexcept {
  poll();
  ring::cpu_relax();
}

void ClientTransport::poll() noexcept {
  if (!state_) return;
  std::lock_guard lock(state_->mutex);
  state_->progress_locked();
}

bool ClientTransport::failed() const noexcept {
  if (!state_) return true;
  std::lock_guard lock(state_->mutex);
  return state_->failed_state;
}

std::string_view ClientTransport::error() const noexcept {
  if (!state_) return {};
  std::lock_guard lock(state_->mutex);
  return state_->error_text;
}

std::size_t ClientTransport::send_capacity() const noexcept {
  return max_message_bytes();
}

std::size_t ClientTransport::receive_capacity() const noexcept {
  return max_message_bytes();
}

std::size_t ClientTransport::max_message_bytes() const noexcept {
  if (!state_) return 0;
  std::lock_guard lock(state_->mutex);
  return state_->negotiated_max_payload;
}

std::size_t ClientTransport::buffer_size_hint() const noexcept {
  return buffer_size_;
}

}  // namespace goblin::core::fabric

#endif  // GOBLIN_HAS_LIBFABRIC
