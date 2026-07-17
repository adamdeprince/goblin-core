#include "goblin/core/rdma_ring.hpp"

#if defined(GOBLIN_HAS_RDMA)

#include "goblin/core/numa.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <infiniband/verbs.h>
#include <rdma/rdma_cma.h>

namespace goblin::core::rdma {
namespace {

constexpr std::uint32_t kDescriptorMagic = 0x47425244;  // GBRD
constexpr std::uint16_t kDescriptorVersion = 2;
constexpr std::size_t kDescriptorBytes = 40;
constexpr std::size_t kRegionHeaderBytes = 128;
constexpr std::size_t kConsumedOffset = 0;
constexpr std::size_t kCreditReadbackOffset = 64;
constexpr std::uint64_t kSequenceLimit = std::uint64_t{1} << 56;
constexpr std::uint64_t kWrTagMask = std::uint64_t{3} << 62;
constexpr std::uint64_t kWrSequenceMask = ~kWrTagMask;
constexpr std::uint64_t kWrWrite = std::uint64_t{1} << 62;
constexpr std::uint64_t kWrCreditRead = std::uint64_t{2} << 62;
constexpr std::uint64_t kSignalEvery = 32;
constexpr std::uint32_t kSendQueueDepth = 512;
constexpr std::uint32_t kCqDepth = 1024;

static_assert(kMaxPayload + sizeof(std::uint64_t) <= 220);
static_assert(kCommitOffset % alignof(std::uint64_t) == 0);
static_assert(kRegionHeaderBytes % 64 == 0);

[[nodiscard]] std::size_t align_up(std::size_t value,
                                   std::size_t alignment) noexcept {
  return (value + alignment - 1) / alignment * alignment;
}

[[nodiscard]] std::string errno_message(std::string_view operation,
                                        int error = errno) {
  return std::string(operation) + ": " + std::strerror(error);
}

[[nodiscard]] bool compute_region_geometry(
    std::uint64_t requested_ring_bytes, std::uint32_t& slot_count,
    std::size_t& mapped_bytes, std::string& error) {
  const std::uint64_t slots = requested_ring_bytes / kSlotStride;
  if (slots < 2 || slots > std::numeric_limits<std::uint32_t>::max()) {
    error = "RDMA ring must hold between 2 and 2^32-1 slots";
    return false;
  }
  const std::uint64_t unrounded =
      kRegionHeaderBytes + slots * static_cast<std::uint64_t>(kSlotStride);
  const long page_result = ::sysconf(_SC_PAGESIZE);
  if (page_result <= 0) {
    error = errno_message("read native page size");
    return false;
  }
  const auto page = static_cast<std::size_t>(page_result);
  if (unrounded > std::numeric_limits<std::size_t>::max() - (page - 1)) {
    error = "RDMA ring allocation is too large";
    return false;
  }
  const std::size_t rounded =
      align_up(static_cast<std::size_t>(unrounded), page);
  if (rounded > std::numeric_limits<std::uint32_t>::max()) {
    error = "RDMA ring allocation exceeds the wire descriptor limit";
    return false;
  }
  slot_count = static_cast<std::uint32_t>(slots);
  mapped_bytes = rounded;
  return true;
}

[[nodiscard]] constexpr std::uint64_t byteswap64(
    std::uint64_t value) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  return __builtin_bswap64(value);
#else
  return ((value & 0x00000000000000ffULL) << 56) |
         ((value & 0x000000000000ff00ULL) << 40) |
         ((value & 0x0000000000ff0000ULL) << 24) |
         ((value & 0x00000000ff000000ULL) << 8) |
         ((value & 0x000000ff00000000ULL) >> 8) |
         ((value & 0x0000ff0000000000ULL) >> 24) |
         ((value & 0x00ff000000000000ULL) >> 40) |
         ((value & 0xff00000000000000ULL) >> 56);
#endif
}

[[nodiscard]] std::uint64_t host_to_be64(std::uint64_t value) noexcept {
  if constexpr (std::endian::native == std::endian::little) {
    return byteswap64(value);
  }
  return value;
}

[[nodiscard]] std::uint64_t be64_to_host(std::uint64_t value) noexcept {
  return host_to_be64(value);
}

[[nodiscard]] std::uint64_t host_to_le64(std::uint64_t value) noexcept {
  if constexpr (std::endian::native == std::endian::big) {
    return byteswap64(value);
  }
  return value;
}

[[nodiscard]] std::uint64_t le64_to_host(std::uint64_t value) noexcept {
  return host_to_le64(value);
}

void put_u16(std::array<std::byte, kDescriptorBytes>& out, std::size_t offset,
             std::uint16_t value) noexcept {
  value = htons(value);
  std::memcpy(out.data() + offset, &value, sizeof(value));
}

void put_u32(std::array<std::byte, kDescriptorBytes>& out, std::size_t offset,
             std::uint32_t value) noexcept {
  value = htonl(value);
  std::memcpy(out.data() + offset, &value, sizeof(value));
}

void put_u64(std::array<std::byte, kDescriptorBytes>& out, std::size_t offset,
             std::uint64_t value) noexcept {
  value = host_to_be64(value);
  std::memcpy(out.data() + offset, &value, sizeof(value));
}

[[nodiscard]] std::uint16_t get_u16(const std::byte* data,
                                    std::size_t offset) noexcept {
  std::uint16_t value = 0;
  std::memcpy(&value, data + offset, sizeof(value));
  return ntohs(value);
}

[[nodiscard]] std::uint32_t get_u32(const std::byte* data,
                                    std::size_t offset) noexcept {
  std::uint32_t value = 0;
  std::memcpy(&value, data + offset, sizeof(value));
  return ntohl(value);
}

[[nodiscard]] std::uint64_t get_u64(const std::byte* data,
                                    std::size_t offset) noexcept {
  std::uint64_t value = 0;
  std::memcpy(&value, data + offset, sizeof(value));
  return be64_to_host(value);
}

struct Descriptor {
  std::uint64_t address{0};
  std::uint64_t nonce{0};
  std::uint32_t rkey{0};
  std::uint32_t region_bytes{0};
  std::uint32_t slot_count{0};
};

[[nodiscard]] std::array<std::byte, kDescriptorBytes> encode_descriptor(
    const Descriptor& descriptor) noexcept {
  std::array<std::byte, kDescriptorBytes> bytes{};
  put_u32(bytes, 0, kDescriptorMagic);
  put_u16(bytes, 4, kDescriptorVersion);
  put_u16(bytes, 6, static_cast<std::uint16_t>(kSlotStride));
  put_u32(bytes, 8, descriptor.slot_count);
  put_u16(bytes, 12, static_cast<std::uint16_t>(kMaxPayload));
  put_u16(bytes, 14, 0);
  put_u64(bytes, 16, descriptor.address);
  put_u32(bytes, 24, descriptor.rkey);
  put_u32(bytes, 28, descriptor.region_bytes);
  put_u64(bytes, 32, descriptor.nonce);
  return bytes;
}

[[nodiscard]] bool decode_descriptor(const void* private_data,
                                     std::size_t private_data_len,
                                     Descriptor& descriptor,
                                     std::string& error) {
  // RDMA-CM documents that the transport may deliver more private data than the
  // application requested. Native InfiniBand presents the full 56-byte CM field
  // here even though both peers supplied a 40-byte descriptor, so validate and
  // decode our fixed prefix while ignoring the provider padding.
  if (private_data == nullptr || private_data_len < kDescriptorBytes) {
    error = "RDMA peer supplied a " + std::to_string(private_data_len) +
            "-byte ring descriptor; expected at least " +
            std::to_string(kDescriptorBytes);
    return false;
  }
  const auto* bytes = static_cast<const std::byte*>(private_data);
  if (get_u32(bytes, 0) != kDescriptorMagic ||
      get_u16(bytes, 4) != kDescriptorVersion ||
      get_u16(bytes, 6) != kSlotStride || get_u16(bytes, 12) != kMaxPayload) {
    error = "RDMA peer supplied an incompatible ring descriptor";
    return false;
  }
  descriptor.slot_count = get_u32(bytes, 8);
  descriptor.address = get_u64(bytes, 16);
  descriptor.rkey = get_u32(bytes, 24);
  descriptor.region_bytes = get_u32(bytes, 28);
  descriptor.nonce = get_u64(bytes, 32);
  const std::uint64_t required =
      kRegionHeaderBytes + static_cast<std::uint64_t>(descriptor.slot_count) *
                               kSlotStride;
  if (descriptor.slot_count < 2 || descriptor.address == 0 ||
      required > descriptor.region_bytes) {
    error = "RDMA peer supplied an invalid ring geometry";
    return false;
  }
  return true;
}

[[nodiscard]] int hca_numa_node(ibv_context* context) noexcept {
  if (context == nullptr || context->device == nullptr) {
    return -1;
  }
  char path[256];
  const int length = std::snprintf(
      path, sizeof(path), "/sys/class/infiniband/%s/device/numa_node",
      ibv_get_device_name(context->device));
  if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path)) {
    return -1;
  }
  std::FILE* file = std::fopen(path, "re");
  if (file == nullptr) {
    return -1;
  }
  int node = -1;
  (void)std::fscanf(file, "%d", &node);
  std::fclose(file);
  return node;
}

struct CmEventCopy {
  rdma_cm_event_type type{RDMA_CM_EVENT_ADDR_ERROR};
  int status{0};
  rdma_cm_id* id{nullptr};
  std::array<std::byte, 256> private_data{};
  std::size_t private_data_len{0};
};

[[nodiscard]] bool copy_and_ack_event(rdma_cm_event* event,
                                      CmEventCopy& copy) noexcept {
  copy.type = event->event;
  copy.status = event->status;
  copy.id = event->id;
  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST ||
      event->event == RDMA_CM_EVENT_ESTABLISHED ||
      event->event == RDMA_CM_EVENT_REJECTED) {
    copy.private_data_len = std::min<std::size_t>(
        event->param.conn.private_data_len, copy.private_data.size());
    if (copy.private_data_len != 0 && event->param.conn.private_data != nullptr) {
      std::memcpy(copy.private_data.data(), event->param.conn.private_data,
                  copy.private_data_len);
    }
  }
  return rdma_ack_cm_event(event) == 0;
}

[[nodiscard]] bool wait_for_event(rdma_event_channel* channel,
                                  rdma_cm_event_type expected,
                                  std::chrono::milliseconds timeout,
                                  CmEventCopy& copy, std::string& error) {
  pollfd descriptor{.fd = channel->fd, .events = POLLIN, .revents = 0};
  const int ready = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
  if (ready <= 0) {
    error = ready == 0 ? "timed out waiting for an RDMA-CM event"
                       : errno_message("poll RDMA-CM channel");
    return false;
  }
  rdma_cm_event* event = nullptr;
  if (rdma_get_cm_event(channel, &event) != 0) {
    error = errno_message("read RDMA-CM event");
    return false;
  }
  if (!copy_and_ack_event(event, copy)) {
    error = errno_message("acknowledge RDMA-CM event");
    return false;
  }
  if (copy.type != expected) {
    error = "expected RDMA-CM event " +
            std::string(rdma_event_str(expected)) + ", received " +
            rdma_event_str(copy.type);
    if (copy.status != 0) {
      error += " (status " + std::to_string(copy.status) + ')';
    }
    return false;
  }
  if (copy.status != 0) {
    error = "RDMA-CM event " + std::string(rdma_event_str(copy.type)) +
            " failed with status " + std::to_string(copy.status);
    return false;
  }
  return true;
}

}  // namespace

struct Connection::Impl {
  rdma_cm_id* id{nullptr};
  rdma_event_channel* owned_channel{nullptr};
  ibv_pd* pd{nullptr};
  ibv_cq* send_cq{nullptr};
  ibv_mr* region_mr{nullptr};
  std::byte* region{nullptr};
  std::size_t region_bytes{0};
  std::uint32_t local_slot_count{0};
  Descriptor remote{};

  bool established_state{false};
  bool disconnected_state{false};
  bool failed_state{false};
  std::string error_text;

  std::uint64_t receive_sequence{1};
  std::uint64_t send_sequence{1};
  std::uint64_t cached_remote_consumed{0};
  std::uint64_t posted_wqes{0};
  std::uint64_t completed_wqes{0};
  std::uint32_t send_queue_depth{0};
  std::uint32_t signal_every{1};

  ~Impl() { reset(); }

  void fail(std::string message) noexcept {
    if (!failed_state) {
      error_text = std::move(message);
    }
    failed_state = true;
  }

  void reset() noexcept {
    if (id != nullptr && established_state && !disconnected_state) {
      (void)rdma_disconnect(id);
    }
    if (id != nullptr && id->qp != nullptr) {
      rdma_destroy_qp(id);
    }
    if (region_mr != nullptr) {
      (void)ibv_dereg_mr(region_mr);
      region_mr = nullptr;
    }
    if (send_cq != nullptr) {
      (void)ibv_destroy_cq(send_cq);
      send_cq = nullptr;
    }
    if (pd != nullptr) {
      (void)ibv_dealloc_pd(pd);
      pd = nullptr;
    }
    if (id != nullptr) {
      (void)rdma_destroy_id(id);
      id = nullptr;
    }
    if (owned_channel != nullptr) {
      rdma_destroy_event_channel(owned_channel);
      owned_channel = nullptr;
    }
    if (region != nullptr) {
      (void)::munmap(region, region_bytes);
      region = nullptr;
      region_bytes = 0;
    }
  }

  [[nodiscard]] bool initialize(std::uint64_t requested_ring_bytes,
                                int requested_numa_node = -1) {
    if (id == nullptr || id->verbs == nullptr) {
      fail("RDMA-CM did not select a verbs device");
      return false;
    }
    if (!compute_region_geometry(requested_ring_bytes, local_slot_count,
                                 region_bytes, error_text)) {
      failed_state = true;
      return false;
    }
    void* mapping = ::mmap(nullptr, region_bytes, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) {
      region = nullptr;
      fail(errno_message("mmap RDMA ring"));
      return false;
    }
    region = static_cast<std::byte*>(mapping);

    const int node = requested_numa_node >= 0
                         ? requested_numa_node
                         : hca_numa_node(id->verbs);
    if (node >= 0 && !numa::bind_range(region, region_bytes, node)) {
      fail(errno_message("bind RDMA ring to selected NUMA node"));
      return false;
    }
    std::memset(region, 0, region_bytes);  // prefault before registration
    if (node >= 0 && !numa::all_on_node(region, region_bytes, node)) {
      fail("RDMA ring could not be placed entirely on selected NUMA node " +
           std::to_string(node));
      return false;
    }

    pd = ibv_alloc_pd(id->verbs);
    if (pd == nullptr) {
      fail(errno_message("allocate RDMA protection domain"));
      return false;
    }
    send_cq = ibv_create_cq(id->verbs, kCqDepth, nullptr, nullptr, 0);
    if (send_cq == nullptr) {
      fail(errno_message("create RDMA send completion queue"));
      return false;
    }
    region_mr = ibv_reg_mr(pd, region, region_bytes,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                               IBV_ACCESS_REMOTE_READ);
    if (region_mr == nullptr) {
      fail(errno_message("register RDMA ring memory"));
      return false;
    }

    ibv_qp_init_attr qp{};
    qp.send_cq = send_cq;
    qp.recv_cq = send_cq;
    qp.cap.max_send_wr = kSendQueueDepth;
    qp.cap.max_recv_wr = 1;
    qp.cap.max_send_sge = 1;
    qp.cap.max_recv_sge = 1;
    qp.cap.max_inline_data =
        static_cast<std::uint32_t>(kMaxPayload + sizeof(std::uint64_t));
    qp.qp_type = IBV_QPT_RC;
    qp.sq_sig_all = 0;
    if (rdma_create_qp(id, pd, &qp) != 0) {
      fail(errno_message("create RC RDMA queue pair"));
      return false;
    }
    if (qp.cap.max_inline_data < kMaxPayload + sizeof(std::uint64_t)) {
      fail("RDMA device cannot inline the 200-byte Goblin slot write");
      return false;
    }
    send_queue_depth = qp.cap.max_send_wr;
    if (send_queue_depth < 2) {
      fail("RDMA device supplied an unusable send queue");
      return false;
    }
    signal_every = std::min<std::uint32_t>(
        static_cast<std::uint32_t>(kSignalEvery), send_queue_depth / 2);
    return true;
  }

  [[nodiscard]] Descriptor local_descriptor() const noexcept {
    Descriptor descriptor;
    descriptor.address = reinterpret_cast<std::uintptr_t>(region);
    descriptor.nonce = reinterpret_cast<std::uintptr_t>(this) ^
                       static_cast<std::uint64_t>(::getpid());
    descriptor.rkey = region_mr->rkey;
    descriptor.region_bytes = static_cast<std::uint32_t>(region_bytes);
    descriptor.slot_count = local_slot_count;
    return descriptor;
  }

  void reap_completions() noexcept {
    if (send_cq == nullptr || failed_state) {
      return;
    }
    std::array<ibv_wc, 16> completions{};
    for (;;) {
      const int count = ibv_poll_cq(send_cq,
                                   static_cast<int>(completions.size()),
                                   completions.data());
      if (count < 0) {
        fail("poll RDMA send completion queue failed");
        return;
      }
      if (count == 0) {
        return;
      }
      for (int i = 0; i < count; ++i) {
        if (completions[static_cast<std::size_t>(i)].status != IBV_WC_SUCCESS) {
          fail("RDMA work request failed: " + std::string(ibv_wc_status_str(
                                                    completions[static_cast<std::size_t>(i)]
                                                        .status)));
          return;
        }
        completed_wqes = std::max(
            completed_wqes,
            completions[static_cast<std::size_t>(i)].wr_id & kWrSequenceMask);
      }
    }
  }

  [[nodiscard]] bool wait_for_completion(std::uint64_t sequence) noexcept {
    while (!failed_state && !disconnected_state && completed_wqes < sequence) {
      reap_completions();
      if (completed_wqes < sequence) {
        ring::cpu_relax();
      }
    }
    return !failed_state && !disconnected_state;
  }

  [[nodiscard]] bool reserve_wqe() noexcept {
    reap_completions();
    if (posted_wqes - completed_wqes < send_queue_depth - 1) {
      return true;
    }
    return wait_for_completion(completed_wqes + 1);
  }

  [[nodiscard]] bool refresh_remote_consumed() noexcept {
    if (!established_state || failed_state || disconnected_state ||
        !reserve_wqe()) {
      return false;
    }
    auto* destination = reinterpret_cast<std::uint64_t*>(
        region + kCreditReadbackOffset);
    ibv_sge sge{};
    sge.addr = reinterpret_cast<std::uintptr_t>(destination);
    sge.length = sizeof(*destination);
    sge.lkey = region_mr->lkey;

    ibv_send_wr request{};
    request.wr_id = kWrCreditRead | (++posted_wqes);
    request.sg_list = &sge;
    request.num_sge = 1;
    request.opcode = IBV_WR_RDMA_READ;
    request.send_flags = IBV_SEND_SIGNALED;
    request.wr.rdma.remote_addr = remote.address + kConsumedOffset;
    request.wr.rdma.rkey = remote.rkey;
    ibv_send_wr* bad = nullptr;
    const int result = ibv_post_send(id->qp, &request, &bad);
    if (result != 0) {
      --posted_wqes;
      fail(errno_message("post RDMA credit read", result));
      return false;
    }
    const std::uint64_t request_sequence = request.wr_id & kWrSequenceMask;
    if (!wait_for_completion(request_sequence)) {
      return false;
    }
    const std::uint64_t consumed = le64_to_host(__atomic_load_n(
        destination, __ATOMIC_ACQUIRE));
    if (consumed < cached_remote_consumed || consumed >= send_sequence) {
      fail("RDMA peer returned an invalid consumed sequence");
      return false;
    }
    cached_remote_consumed = consumed;
    return true;
  }

  [[nodiscard]] bool post_fragment(std::string_view payload) noexcept {
    if (payload.empty() || payload.size() > kMaxPayload ||
        send_sequence >= kSequenceLimit || !reserve_wqe()) {
      if (send_sequence >= kSequenceLimit) {
        fail("RDMA ring sequence space exhausted");
      }
      return false;
    }
    const std::uint64_t used =
        (send_sequence - 1) - cached_remote_consumed;
    if (used >= remote.slot_count) {
      if (!refresh_remote_consumed() ||
          (send_sequence - 1) - cached_remote_consumed >= remote.slot_count) {
        return false;
      }
    }

    std::array<std::byte, kMaxPayload + sizeof(std::uint64_t)> inline_data{};
    std::memcpy(inline_data.data(), payload.data(), payload.size());
    const std::uint64_t commit = host_to_le64(
        (send_sequence << 8) | static_cast<std::uint64_t>(payload.size()));
    std::memcpy(inline_data.data() + payload.size(), &commit, sizeof(commit));

    const std::uint64_t slot = (send_sequence - 1) % remote.slot_count;
    const std::uint64_t remote_commit =
        remote.address + kRegionHeaderBytes + slot * kSlotStride + kCommitOffset;

    ibv_sge sge{};
    sge.addr = reinterpret_cast<std::uintptr_t>(inline_data.data());
    sge.length = static_cast<std::uint32_t>(payload.size() + sizeof(commit));
    sge.lkey = 0;  // inline bytes are copied into the WQE before this call returns

    const std::uint64_t wqe_sequence = ++posted_wqes;
    const bool signaled = (wqe_sequence % signal_every) == 0;
    ibv_send_wr request{};
    request.wr_id = kWrWrite | wqe_sequence;
    request.sg_list = &sge;
    request.num_sge = 1;
    request.opcode = IBV_WR_RDMA_WRITE;
    request.send_flags = IBV_SEND_INLINE | (signaled ? IBV_SEND_SIGNALED : 0);
    request.wr.rdma.remote_addr = remote_commit - payload.size();
    request.wr.rdma.rkey = remote.rkey;
    ibv_send_wr* bad = nullptr;
    const int result = ibv_post_send(id->qp, &request, &bad);
    if (result != 0) {
      --posted_wqes;
      fail(errno_message("post inline RDMA write", result));
      return false;
    }
    ++send_sequence;
    return true;
  }
};

Connection::Connection(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
Connection::Connection(Connection&&) noexcept = default;
Connection& Connection::operator=(Connection&&) noexcept = default;
Connection::~Connection() = default;

bool Connection::established() const noexcept {
  return impl_ != nullptr && impl_->established_state;
}
bool Connection::disconnected() const noexcept {
  return impl_ == nullptr || impl_->disconnected_state;
}
bool Connection::failed() const noexcept {
  return impl_ == nullptr || impl_->failed_state;
}
std::string_view Connection::error() const noexcept {
  return impl_ == nullptr ? std::string_view{"invalid RDMA connection"}
                          : std::string_view{impl_->error_text};
}

std::optional<std::string_view> Connection::peek() noexcept {
  if (!established() || failed() || disconnected()) {
    return std::nullopt;
  }
  const std::uint64_t slot =
      (impl_->receive_sequence - 1) % impl_->local_slot_count;
  auto* commit_address = reinterpret_cast<std::uint64_t*>(
      impl_->region + kRegionHeaderBytes + slot * kSlotStride + kCommitOffset);
  const std::uint64_t commit =
      le64_to_host(__atomic_load_n(commit_address, __ATOMIC_ACQUIRE));
  const std::uint64_t sequence = commit >> 8;
  if (sequence < impl_->receive_sequence) {
    return std::nullopt;
  }
  if (sequence != impl_->receive_sequence) {
    impl_->fail("RDMA ring sequence jumped from " +
                std::to_string(impl_->receive_sequence) + " to " +
                std::to_string(sequence));
    return std::nullopt;
  }
  const std::size_t length = static_cast<std::size_t>(commit & 0xffU);
  if (length == 0 || length > kMaxPayload) {
    impl_->fail("RDMA ring published an invalid fragment length");
    return std::nullopt;
  }
  const auto* payload = reinterpret_cast<const char*>(commit_address) - length;
  return std::string_view(payload, length);
}

void Connection::pop() noexcept {
  if (impl_ == nullptr || impl_->failed_state || impl_->disconnected_state) {
    return;
  }
  const std::uint64_t consumed = impl_->receive_sequence++;
  auto* word = reinterpret_cast<std::uint64_t*>(impl_->region + kConsumedOffset);
  __atomic_store_n(word, host_to_le64(consumed), __ATOMIC_RELEASE);
}

bool Connection::try_push(std::string_view payload) noexcept {
  return impl_ != nullptr && impl_->established_state &&
         !impl_->failed_state && !impl_->disconnected_state &&
         impl_->post_fragment(payload);
}

std::size_t Connection::inbound_capacity() const noexcept {
  return impl_ == nullptr
             ? 0
             : static_cast<std::size_t>(impl_->local_slot_count) * kMaxPayload;
}

std::size_t Connection::outbound_capacity() const noexcept {
  return impl_ == nullptr
             ? 0
             : static_cast<std::size_t>(impl_->remote.slot_count) * kMaxPayload;
}

void Connection::disconnect() noexcept {
  if (impl_ != nullptr && impl_->id != nullptr &&
      impl_->established_state && !impl_->disconnected_state) {
    if (rdma_disconnect(impl_->id) != 0) {
      impl_->fail(errno_message("disconnect RDMA connection"));
    }
  }
}

struct ServerListener::Impl {
  rdma_event_channel* channel{nullptr};
  rdma_cm_id* listen_id{nullptr};
  std::uint64_t ring_bytes{0};
  int numa_node{-1};
  std::string error_text;

  ~Impl() {
    if (listen_id != nullptr) {
      (void)rdma_destroy_id(listen_id);
    }
    if (channel != nullptr) {
      rdma_destroy_event_channel(channel);
    }
  }
};

ServerListener::ServerListener(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}
ServerListener::ServerListener(ServerListener&&) noexcept = default;
ServerListener& ServerListener::operator=(ServerListener&&) noexcept = default;
ServerListener::~ServerListener() = default;

std::unique_ptr<ServerListener> ServerListener::create(
    std::string_view address, std::uint16_t port, std::uint64_t ring_bytes,
    int backlog, int numa_node, std::string& error) {
  auto impl = std::make_unique<Impl>();
  std::uint32_t ignored_slots = 0;
  std::size_t ignored_bytes = 0;
  if (!compute_region_geometry(ring_bytes, ignored_slots, ignored_bytes, error)) {
    return nullptr;
  }
  impl->ring_bytes = ring_bytes;
  impl->numa_node = numa_node;
  impl->channel = rdma_create_event_channel();
  if (impl->channel == nullptr) {
    error = errno_message("create RDMA-CM event channel");
    return nullptr;
  }
  if (rdma_create_id(impl->channel, &impl->listen_id, impl.get(), RDMA_PS_TCP) !=
      0) {
    error = errno_message("create RDMA-CM listener id");
    return nullptr;
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  addrinfo* addresses = nullptr;
  const std::string address_text(address);
  const std::string port_text = std::to_string(port);
  const int lookup = ::getaddrinfo(address_text.c_str(), port_text.c_str(),
                                   &hints, &addresses);
  if (lookup != 0) {
    error = "resolve RDMA bind address: " + std::string(gai_strerror(lookup));
    return nullptr;
  }
  const int bind_result = rdma_bind_addr(impl->listen_id, addresses->ai_addr);
  ::freeaddrinfo(addresses);
  if (bind_result != 0) {
    error = errno_message("bind RDMA-CM listener");
    return nullptr;
  }
  if (rdma_listen(impl->listen_id, backlog) != 0) {
    error = errno_message("listen for RDMA-CM connections");
    return nullptr;
  }
  const int flags = ::fcntl(impl->channel->fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(impl->channel->fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    error = errno_message("make RDMA-CM event channel non-blocking");
    return nullptr;
  }
  return std::unique_ptr<ServerListener>(
      new ServerListener(std::move(impl)));
}

ListenerPoll ServerListener::poll() noexcept {
  ListenerPoll result;
  if (impl_ == nullptr || impl_->channel == nullptr) {
    return result;
  }
  rdma_cm_event* raw = nullptr;
  if (rdma_get_cm_event(impl_->channel, &raw) != 0) {
    if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
      impl_->error_text = errno_message("read RDMA-CM listener event");
    }
    return result;
  }
  CmEventCopy event;
  if (!copy_and_ack_event(raw, event)) {
    impl_->error_text = errno_message("acknowledge RDMA-CM listener event");
    return result;
  }
  result.progressed = true;

  if (event.id == impl_->listen_id) {
    impl_->error_text = "RDMA listener received " +
                        std::string(rdma_event_str(event.type));
    if (event.status != 0) {
      impl_->error_text += " (status " + std::to_string(event.status) + ')';
    }
    return result;
  }

  if (event.type == RDMA_CM_EVENT_CONNECT_REQUEST) {
    auto connection_impl = std::make_unique<Connection::Impl>();
    connection_impl->id = event.id;
    connection_impl->id->context = connection_impl.get();
    if (!decode_descriptor(event.private_data.data(), event.private_data_len,
                           connection_impl->remote,
                           connection_impl->error_text) ||
        !connection_impl->initialize(impl_->ring_bytes, impl_->numa_node)) {
      connection_impl->failed_state = true;
      (void)rdma_reject(event.id, nullptr, 0);
      result.connection = std::unique_ptr<Connection>(
          new Connection(std::move(connection_impl)));
      return result;
    }
    const auto descriptor =
        encode_descriptor(connection_impl->local_descriptor());
    rdma_conn_param parameters{};
    parameters.private_data = descriptor.data();
    parameters.private_data_len = descriptor.size();
    parameters.responder_resources = 1;
    parameters.initiator_depth = 1;
    parameters.retry_count = 7;
    parameters.rnr_retry_count = 7;
    if (rdma_accept(event.id, &parameters) != 0) {
      connection_impl->fail(errno_message("accept RDMA-CM connection"));
    }
    result.connection = std::unique_ptr<Connection>(
        new Connection(std::move(connection_impl)));
    return result;
  }

  auto* connection = static_cast<Connection::Impl*>(event.id->context);
  if (connection == nullptr) {
    impl_->error_text = "RDMA-CM event has no connection context";
    return result;
  }
  switch (event.type) {
    case RDMA_CM_EVENT_ESTABLISHED:
      connection->established_state = true;
      break;
    case RDMA_CM_EVENT_DISCONNECTED:
      connection->disconnected_state = true;
      break;
    case RDMA_CM_EVENT_DEVICE_REMOVAL:
      connection->disconnected_state = true;
      connection->fail("RDMA device was removed");
      break;
    case RDMA_CM_EVENT_REJECTED:
    case RDMA_CM_EVENT_CONNECT_ERROR:
    case RDMA_CM_EVENT_UNREACHABLE:
      connection->disconnected_state = true;
      connection->fail("RDMA-CM connection failed: " +
                       std::string(rdma_event_str(event.type)));
      break;
    case RDMA_CM_EVENT_TIMEWAIT_EXIT:
      connection->disconnected_state = true;
      break;
    default:
      break;
  }
  return result;
}

std::string_view ServerListener::error() const noexcept {
  return impl_ == nullptr ? std::string_view{"invalid RDMA listener"}
                          : std::string_view{impl_->error_text};
}

std::optional<ClientTransport> ClientTransport::open(
    std::string_view address, std::uint16_t port, std::uint64_t ring_bytes,
    ms timeout, std::size_t buffer_size, std::string* error_out) {
  std::string error;
  std::uint32_t ignored_slots = 0;
  std::size_t ignored_bytes = 0;
  if (!compute_region_geometry(ring_bytes, ignored_slots, ignored_bytes, error)) {
    if (error_out != nullptr) {
      *error_out = error;
    }
    return std::nullopt;
  }
  auto impl = std::make_unique<Connection::Impl>();
  impl->owned_channel = rdma_create_event_channel();
  if (impl->owned_channel == nullptr) {
    error = errno_message("create RDMA-CM client event channel");
  } else if (rdma_create_id(impl->owned_channel, &impl->id, impl.get(),
                            RDMA_PS_TCP) != 0) {
    error = errno_message("create RDMA-CM client id");
  }

  addrinfo* addresses = nullptr;
  if (error.empty()) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    const std::string address_text(address);
    const std::string port_text = std::to_string(port);
    const int lookup = ::getaddrinfo(address_text.c_str(), port_text.c_str(),
                                     &hints, &addresses);
    if (lookup != 0) {
      error = "resolve RDMA server address: " +
              std::string(gai_strerror(lookup));
    }
  }

  CmEventCopy event;
  if (error.empty() &&
      rdma_resolve_addr(impl->id, nullptr, addresses->ai_addr,
                        static_cast<int>(timeout.count())) != 0) {
    error = errno_message("resolve RDMA-CM address");
  }
  if (addresses != nullptr) {
    ::freeaddrinfo(addresses);
  }
  if (error.empty() &&
      !wait_for_event(impl->owned_channel, RDMA_CM_EVENT_ADDR_RESOLVED,
                      timeout, event, error)) {
    // wait_for_event supplied the diagnostic.
  }
  if (error.empty() &&
      rdma_resolve_route(impl->id, static_cast<int>(timeout.count())) != 0) {
    error = errno_message("resolve RDMA-CM route");
  }
  if (error.empty() &&
      !wait_for_event(impl->owned_channel, RDMA_CM_EVENT_ROUTE_RESOLVED,
                      timeout, event, error)) {
    // wait_for_event supplied the diagnostic.
  }
  if (error.empty() && !impl->initialize(ring_bytes)) {
    error = impl->error_text;
  }

  std::array<std::byte, kDescriptorBytes> descriptor{};
  if (error.empty()) {
    descriptor = encode_descriptor(impl->local_descriptor());
    rdma_conn_param parameters{};
    parameters.private_data = descriptor.data();
    parameters.private_data_len = descriptor.size();
    parameters.responder_resources = 1;
    parameters.initiator_depth = 1;
    parameters.retry_count = 7;
    parameters.rnr_retry_count = 7;
    if (rdma_connect(impl->id, &parameters) != 0) {
      error = errno_message("connect RDMA-CM endpoint");
    }
  }
  if (error.empty() &&
      !wait_for_event(impl->owned_channel, RDMA_CM_EVENT_ESTABLISHED,
                      timeout, event, error)) {
    // wait_for_event supplied the diagnostic.
  }
  if (error.empty() &&
      !decode_descriptor(event.private_data.data(), event.private_data_len,
                         impl->remote, error)) {
    // decode_descriptor supplied the diagnostic.
  }
  if (!error.empty()) {
    if (error_out != nullptr) {
      *error_out = error;
    }
    return std::nullopt;
  }
  impl->established_state = true;
  auto connection =
      std::unique_ptr<Connection>(new Connection(std::move(impl)));
  return ClientTransport(std::move(connection), buffer_size);
}

}  // namespace goblin::core::rdma

#endif  // GOBLIN_HAS_RDMA
