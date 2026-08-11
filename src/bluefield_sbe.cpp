#include "bluefield_sbe.hpp"

#include "goblin/core/goblin_protocol.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin_sbe/ErrorReply.h"
#include "goblin_sbe/IntReply.h"
#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/PSubscribe.h"
#include "goblin_sbe/PUnsubscribe.h"
#include "goblin_sbe/PubSub.h"
#include "goblin_sbe/PubSubPush.h"
#include "goblin_sbe/Publish.h"
#include "goblin_sbe/StatusReply.h"
#include "goblin_sbe/Subscribe.h"
#include "goblin_sbe/Unsubscribe.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <memory>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>
#include <utility>
#include <vector>

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace goblin::core::bluefield {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;

constexpr Milliseconds kTimeout{5000};
constexpr std::size_t kInitialBufferBytes = 16U * 1024U;
constexpr std::size_t kMaximumFrameBytes = 128U * 1024U * 1024U;

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

[[nodiscard]] std::string socket_error(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

[[nodiscard]] bool set_nonblocking(int fd, std::string& error) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0 || ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    error = socket_error("fcntl(O_NONBLOCK)");
    return false;
  }
  return true;
}

[[nodiscard]] bool finish_connect(int fd, Clock::time_point deadline,
                                  std::string& error) {
  for (;;) {
    const auto now = Clock::now();
    if (now >= deadline) {
      error = "connect timed out";
      return false;
    }
    const auto remaining =
        std::chrono::duration_cast<Milliseconds>(deadline - now);
    const int timeout = static_cast<int>(std::clamp<long long>(
        remaining.count(), 1, std::numeric_limits<int>::max()));
    pollfd event{fd, POLLOUT, 0};
    const int ready = ::poll(&event, 1, timeout);
    if (ready < 0 && errno == EINTR) continue;
    if (ready < 0) {
      error = socket_error("poll(connect)");
      return false;
    }
    if (ready == 0) continue;

    int connect_error = 0;
    socklen_t length = sizeof(connect_error);
    if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &connect_error, &length) != 0) {
      error = socket_error("getsockopt(SO_ERROR)");
      return false;
    }
    if (connect_error != 0) {
      error = "connect: " + std::string(std::strerror(connect_error));
      return false;
    }
    return true;
  }
}

[[nodiscard]] int connect_tcp(std::string_view host, std::uint16_t port,
                              std::string& error) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  addrinfo* addresses = nullptr;
  const std::string host_text(host);
  const std::string port_text = std::to_string(port);
  const int resolved = ::getaddrinfo(host_text.c_str(), port_text.c_str(),
                                     &hints, &addresses);
  if (resolved != 0) {
    error = std::string("getaddrinfo: ") + ::gai_strerror(resolved);
    return -1;
  }
  std::unique_ptr<addrinfo, decltype(&::freeaddrinfo)> guard(addresses,
                                                             &::freeaddrinfo);
  const auto deadline = Clock::now() + kTimeout;
  for (auto* address = addresses; address != nullptr;
       address = address->ai_next) {
    const int fd =
        ::socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0) {
      error = socket_error("socket");
      continue;
    }
    int one = 1;
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef TCP_QUICKACK
    (void)::setsockopt(fd, IPPROTO_TCP, TCP_QUICKACK, &one, sizeof(one));
#endif
    if (!set_nonblocking(fd, error)) {
      (void)::close(fd);
      continue;
    }
    if (::connect(fd, address->ai_addr,
                  static_cast<socklen_t>(address->ai_addrlen)) == 0 ||
        (errno == EINPROGRESS && finish_connect(fd, deadline, error))) {
      return fd;
    }
    if (errno != EINPROGRESS && error.empty()) {
      error = socket_error("connect");
    }
    (void)::close(fd);
  }
  return -1;
}

}  // namespace

class EdgeSbeClient::Impl {
 public:
  explicit Impl(int fd) : fd_(fd) {
    send_buffer_.resize(kInitialBufferBytes);
    inbound_.reserve(kInitialBufferBytes);
    send_all(std::string_view(kGoblinMagicBytes, sizeof(kGoblinMagicBytes)));
  }

  ~Impl() {
    if (fd_ >= 0) (void)::close(fd_);
  }

  void register_edge(std::uint64_t edge_id, bool aggregate) {
    if (edge_id == 0) {
      throw std::invalid_argument("BlueField edge id must be non-zero");
    }
    const std::string id = std::to_string(edge_id);
    const std::string_view role = aggregate ? "aggregate" : "publisher";
    auto message = build<goblin_sbe::PubSub>(id.size() + role.size());
    message.operation(3);
    auto& args = message.argsCount(2);
    args.next().putArg(id.data(), to_u32(id.size()));
    args.next().putArg(role.data(), to_u32(role.size()));
    send_built(message);
    expect_status("BlueField registration");
  }

  void set_subscription(std::string_view name, bool pattern, bool subscribe) {
    if (name.empty()) {
      throw std::invalid_argument("BlueField subscription name is empty");
    }
    if (pattern) {
      if (subscribe) {
        auto message = build<goblin_sbe::PSubscribe>(name.size());
        auto& names = message.patternsCount(1);
        names.next().putPattern(name.data(), to_u32(name.size()));
        send_built(message);
        expect_subscription_ack(EdgePubSubKind::pattern_subscribe);
      } else {
        auto message = build<goblin_sbe::PUnsubscribe>(name.size());
        auto& names = message.patternsCount(1);
        names.next().putPattern(name.data(), to_u32(name.size()));
        send_built(message);
        expect_subscription_ack(EdgePubSubKind::pattern_unsubscribe);
      }
    } else if (subscribe) {
      auto message = build<goblin_sbe::Subscribe>(name.size());
      auto& names = message.channelsCount(1);
      names.next().putChannel(name.data(), to_u32(name.size()));
      send_built(message);
      expect_subscription_ack(EdgePubSubKind::subscribe);
    } else {
      auto message = build<goblin_sbe::Unsubscribe>(name.size());
      auto& names = message.channelsCount(1);
      names.next().putChannel(name.data(), to_u32(name.size()));
      send_built(message);
      expect_subscription_ack(EdgePubSubKind::unsubscribe);
    }
  }

  void set_subscription_weight(std::string_view name, bool pattern,
                               std::uint32_t subscribers) {
    if (subscribers == 0) {
      throw std::invalid_argument(
          "BlueField subscription weight must be non-zero");
    }
    const std::string count = std::to_string(subscribers);
    const std::string_view kind = pattern ? "pattern" : "channel";
    auto message =
        build<goblin_sbe::PubSub>(kind.size() + name.size() + count.size());
    message.operation(4);
    auto& args = message.argsCount(3);
    args.next().putArg(kind.data(), to_u32(kind.size()));
    args.next().putArg(name.data(), to_u32(name.size()));
    args.next().putArg(count.data(), to_u32(count.size()));
    send_built(message);
    expect_status("BlueField subscription-weight update");
  }

  void enqueue_publish(std::string_view channel, std::string_view payload) {
    auto message = build<goblin_sbe::Publish>(channel.size() + payload.size());
    message.putChannel(channel.data(), to_u32(channel.size()));
    message.putPayload(payload.data(), to_u32(payload.size()));
    send_built(message);
    ++outstanding_publish_replies_;
  }

  [[nodiscard]] std::optional<long long> try_read_publish_reply() {
    if (outstanding_publish_replies_ == 0) {
      throw std::logic_error("no BlueField PUBLISH reply is outstanding");
    }
    if (!try_next_frame()) return std::nullopt;
    const auto header = reply_header();
    if (header.templateId() == goblin_sbe::ErrorReply::sbeTemplateId()) {
      throw_reply_error(header);
    }
    if (header.templateId() != goblin_sbe::IntReply::sbeTemplateId()) {
      throw std::runtime_error("unexpected SBE PUBLISH reply type");
    }
    auto reply = decode<goblin_sbe::IntReply>(header);
    --outstanding_publish_replies_;
    return reply.value();
  }

  [[nodiscard]] std::optional<EdgePubSubMessage> try_read_pubsub() {
    last_owned_.reset();
    if (!pending_pushes_.empty()) {
      last_owned_.emplace(std::move(pending_pushes_.front()));
      pending_pushes_.pop_front();
      return owned_view(*last_owned_);
    }
    if (!try_next_frame()) return std::nullopt;
    const auto header = reply_header();
    if (header.templateId() == goblin_sbe::ErrorReply::sbeTemplateId()) {
      throw_reply_error(header);
    }
    if (header.templateId() != goblin_sbe::PubSubPush::sbeTemplateId()) {
      throw std::runtime_error("unexpected SBE aggregate reply type");
    }
    return decode_pubsub(header);
  }

 private:
  struct OwnedPush {
    EdgePubSubKind kind{EdgePubSubKind::message};
    std::uint32_t subscription_count{0};
    std::string pattern;
    std::string channel;
    std::string payload;
  };

  [[nodiscard]] static std::uint32_t to_u32(std::size_t value) {
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("SBE field exceeds uint32 length");
    }
    return static_cast<std::uint32_t>(value);
  }

  template <class Message>
  Message build(std::size_t payload_bytes) {
    if (payload_bytes > kMaximumFrameBytes - 512) {
      throw std::length_error("SBE request is too large");
    }
    const std::size_t needed = payload_bytes + 512;
    if (send_buffer_.size() < needed) send_buffer_.resize(needed);
    Message message;
    message.wrapAndApplyHeader(send_buffer_.data(), kSbeLenPrefix,
                               send_buffer_.size());
    return message;
  }

  template <class Message>
  void send_built(Message& message) {
    const std::size_t body = goblin_sbe::MessageHeader::encodedLength() +
                             static_cast<std::size_t>(message.encodedLength());
    if (body > std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("SBE request exceeds uint32 framing");
    }
    const auto length = static_cast<std::uint32_t>(body);
    std::memcpy(send_buffer_.data(), &length, kSbeLenPrefix);
    send_all(std::string_view(send_buffer_.data(), kSbeLenPrefix + body));
  }

  void send_all(std::string_view bytes) {
    const auto deadline = Clock::now() + kTimeout;
    unsigned spins = 0;
    while (!bytes.empty()) {
      const ssize_t sent =
          ::send(fd_, bytes.data(), bytes.size(), MSG_NOSIGNAL);
      if (sent > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(sent));
        continue;
      }
      if (sent < 0 && errno == EINTR) continue;
      if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        if ((++spins & 63U) == 0 && Clock::now() >= deadline) {
          throw std::runtime_error("timed out writing the SBE host link");
        }
        cpu_relax();
        continue;
      }
      throw std::runtime_error(socket_error("send(SBE)"));
    }
  }

  void begin_receive() {
    last_frame_ = {};
    if (inbound_offset_ == inbound_.size()) {
      inbound_.clear();
      inbound_offset_ = 0;
    } else if (inbound_offset_ != 0 &&
               (inbound_offset_ >= kInitialBufferBytes ||
                inbound_offset_ >= inbound_.size() / 2)) {
      inbound_.erase(0, inbound_offset_);
      inbound_offset_ = 0;
    }
  }

  [[nodiscard]] bool receive_available() {
    bool progressed = false;
    for (;;) {
      const std::size_t old_size = inbound_.size();
      inbound_.resize(old_size + kInitialBufferBytes);
      const ssize_t received =
          ::recv(fd_, inbound_.data() + old_size, kInitialBufferBytes, 0);
      if (received > 0) {
        inbound_.resize(old_size + static_cast<std::size_t>(received));
        progressed = true;
        continue;
      }
      inbound_.resize(old_size);
      if (received == 0) {
        throw std::runtime_error("SBE host link closed");
      }
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return progressed;
      throw std::runtime_error(socket_error("recv(SBE)"));
    }
  }

  [[nodiscard]] bool extract_frame() {
    const std::size_t available = inbound_.size() - inbound_offset_;
    if (available < kSbeLenPrefix) return false;
    std::uint32_t length = 0;
    std::memcpy(&length, inbound_.data() + inbound_offset_, kSbeLenPrefix);
    if (length < goblin_sbe::MessageHeader::encodedLength() ||
        length > kMaximumFrameBytes) {
      throw std::runtime_error("invalid SBE frame length");
    }
    const std::size_t frame_bytes =
        kSbeLenPrefix + static_cast<std::size_t>(length);
    if (available < frame_bytes) return false;
    last_frame_ = std::string_view(
        inbound_.data() + inbound_offset_ + kSbeLenPrefix, length);
    inbound_offset_ += frame_bytes;
    return true;
  }

  [[nodiscard]] bool try_next_frame() {
    begin_receive();
    if (extract_frame()) return true;
    (void)receive_available();
    return extract_frame();
  }

  void read_next_frame() {
    const auto deadline = Clock::now() + kTimeout;
    unsigned spins = 0;
    begin_receive();
    for (;;) {
      if (extract_frame()) return;
      (void)receive_available();
      if (extract_frame()) return;
      if ((++spins & 63U) == 0 && Clock::now() >= deadline) {
        throw std::runtime_error("timed out waiting for an SBE host reply");
      }
      cpu_relax();
    }
  }

  [[nodiscard]] goblin_sbe::MessageHeader reply_header() const {
    return goblin_sbe::MessageHeader(
        const_cast<char*>(last_frame_.data()), last_frame_.size());
  }

  template <class Message>
  [[nodiscard]] Message decode(const goblin_sbe::MessageHeader& header) const {
    Message message;
    message.wrapForDecode(
        const_cast<char*>(last_frame_.data()),
        goblin_sbe::MessageHeader::encodedLength(), header.blockLength(),
        header.version(), last_frame_.size());
    return message;
  }

  [[noreturn]] void throw_reply_error(
      const goblin_sbe::MessageHeader& header) const {
    auto error = decode<goblin_sbe::ErrorReply>(header);
    throw std::runtime_error(std::string(error.getCodeAsStringView()) + " " +
                             std::string(error.getMessageAsStringView()));
  }

  [[nodiscard]] EdgePubSubMessage decode_pubsub(
      const goblin_sbe::MessageHeader& header) const {
    auto reply = decode<goblin_sbe::PubSubPush>(header);
    EdgePubSubMessage message;
    message.kind = static_cast<EdgePubSubKind>(reply.kind());
    message.subscription_count = reply.subscriptionCount();
    message.pattern = reply.getPatternAsStringView();
    message.channel = reply.getChannelAsStringView();
    message.payload = reply.getPayloadAsStringView();
    return message;
  }

  [[nodiscard]] static OwnedPush own(const EdgePubSubMessage& message) {
    OwnedPush result;
    result.kind = message.kind;
    result.subscription_count = message.subscription_count;
    result.pattern.assign(message.pattern.data(), message.pattern.size());
    result.channel.assign(message.channel.data(), message.channel.size());
    result.payload.assign(message.payload.data(), message.payload.size());
    return result;
  }

  [[nodiscard]] static EdgePubSubMessage owned_view(
      const OwnedPush& message) noexcept {
    EdgePubSubMessage result;
    result.kind = message.kind;
    result.subscription_count = message.subscription_count;
    result.pattern = message.pattern;
    result.channel = message.channel;
    result.payload = message.payload;
    return result;
  }

  void expect_status(std::string_view operation) {
    for (;;) {
      read_next_frame();
      const auto header = reply_header();
      if (header.templateId() == goblin_sbe::ErrorReply::sbeTemplateId()) {
        throw_reply_error(header);
      }
      if (header.templateId() == goblin_sbe::PubSubPush::sbeTemplateId()) {
        pending_pushes_.push_back(own(decode_pubsub(header)));
        continue;
      }
      if (header.templateId() != goblin_sbe::StatusReply::sbeTemplateId()) {
        throw std::runtime_error(std::string(operation) +
                                 " returned an unexpected SBE reply");
      }
      auto status = decode<goblin_sbe::StatusReply>(header);
      if (status.getStatusAsStringView() != "OK") {
        throw std::runtime_error(std::string(operation) + " failed");
      }
      return;
    }
  }

  void expect_subscription_ack(EdgePubSubKind expected) {
    for (;;) {
      read_next_frame();
      const auto header = reply_header();
      if (header.templateId() == goblin_sbe::ErrorReply::sbeTemplateId()) {
        throw_reply_error(header);
      }
      if (header.templateId() != goblin_sbe::PubSubPush::sbeTemplateId()) {
        throw std::runtime_error(
            "subscription returned an unexpected SBE reply");
      }
      const auto push = decode_pubsub(header);
      if (push.kind == EdgePubSubKind::message ||
          push.kind == EdgePubSubKind::pattern_message) {
        pending_pushes_.push_back(own(push));
        continue;
      }
      if (push.kind != expected) {
        throw std::runtime_error("subscription returned the wrong acknowledgement");
      }
      return;
    }
  }

  int fd_{-1};
  std::vector<char> send_buffer_;
  std::string inbound_;
  std::size_t inbound_offset_{0};
  std::string_view last_frame_;
  std::deque<OwnedPush> pending_pushes_;
  std::optional<OwnedPush> last_owned_;
  std::size_t outstanding_publish_replies_{0};
};

std::unique_ptr<EdgeSbeClient> EdgeSbeClient::open(
    std::string host, std::uint16_t port, std::string& error) {
  const int fd = connect_tcp(host, port, error);
  if (fd < 0) return nullptr;
  bool caller_owns_fd = true;
  try {
    auto impl = std::make_unique<Impl>(fd);
    caller_owns_fd = false;
    return std::unique_ptr<EdgeSbeClient>(
        new EdgeSbeClient(std::move(impl)));
  } catch (const std::exception& failure) {
    if (caller_owns_fd) (void)::close(fd);
    error = failure.what();
    return nullptr;
  }
}

EdgeSbeClient::EdgeSbeClient(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

EdgeSbeClient::~EdgeSbeClient() = default;

void EdgeSbeClient::register_edge(std::uint64_t edge_id, bool aggregate) {
  impl_->register_edge(edge_id, aggregate);
}

void EdgeSbeClient::set_subscription(std::string_view name, bool pattern,
                                     bool subscribe) {
  impl_->set_subscription(name, pattern, subscribe);
}

void EdgeSbeClient::set_subscription_weight(std::string_view name, bool pattern,
                                            std::uint32_t subscribers) {
  impl_->set_subscription_weight(name, pattern, subscribers);
}

void EdgeSbeClient::enqueue_publish(std::string_view channel,
                                    std::string_view payload) {
  impl_->enqueue_publish(channel, payload);
}

std::optional<long long> EdgeSbeClient::try_read_publish_reply() {
  return impl_->try_read_publish_reply();
}

std::optional<EdgePubSubMessage> EdgeSbeClient::try_read_pubsub() {
  return impl_->try_read_pubsub();
}

}  // namespace goblin::core::bluefield
