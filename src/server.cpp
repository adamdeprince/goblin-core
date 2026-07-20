#include "goblin/core/server.hpp"

#include "goblin/core/auth.hpp"
#include "goblin/core/command.hpp"
#include "goblin/core/goblin_protocol.hpp"
#ifdef GOBLIN_HAS_KAFKA
#include "goblin/core/kafka_ingest.hpp"
#endif
#include "goblin/core/luau_script.hpp"
#include "goblin/core/resp_parser.hpp"
#include "goblin/core/resp_writer.hpp"
#include "goblin/core/replication.hpp"
#include "goblin/core/ring_buffer.hpp"
#ifdef GOBLIN_HAS_RDMA
#include "goblin/core/rdma_ring.hpp"
#endif
#include "goblin/core/exasock.hpp"
#include "goblin/core/script.hpp"
#ifdef GOBLIN_HAS_SBE
#include "goblin/core/sbe_dispatch.hpp"
#include "goblin/core/sbe_frame.hpp"
#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/PSubscribe.h"
#include "goblin_sbe/PUnsubscribe.h"
#include "goblin_sbe/PubSub.h"
#include "goblin_sbe/Publish.h"
#include "goblin_sbe/Subscribe.h"
#include "goblin_sbe/Unsubscribe.h"
#endif
#include "goblin/core/store.hpp"
#include "goblin/core/tcl_script.hpp"
#include "goblin/core/upython_script.hpp"
#include "goblin/core/quickjs_script.hpp"
#include "goblin/core/wren_script.hpp"
#include "pubsub.hpp"
#include "replication_follower.hpp"
#include "transaction.hpp"
#ifdef GOBLIN_HAS_SBE
#include "pubsub_listener.hpp"
#endif

#ifdef GOBLIN_HAS_TLS
#include <openssl/err.h>
#include <openssl/ssl.h>
#endif

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <netdb.h>
#include <optional>
#include <poll.h>
#include <string>
#include <string_view>
#include <stdexcept>
#include <thread>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace goblin::core {
namespace {

constexpr std::size_t kOutputCompactThreshold = 64 * 1024;

#ifdef GOBLIN_HAS_TLS
enum class TlsWant : std::uint8_t {
  Read,
  Write,
};
#endif

struct ReplyBoundary {
  std::uint64_t sequence{0};
  std::size_t end_offset{0};
};

struct BlockedListRequest {
  using Clock = std::chrono::steady_clock;

  std::vector<std::string> fields;
  std::optional<Clock::time_point> deadline;
  bool null_array{false};
};

struct EndpointSession : detail::PubSubSession {
  EndpointSession(std::size_t unsolicited_output_bytes,
                  std::size_t transaction_buffer_bytes,
                  std::uint64_t assigned_connection_id,
                  bool require_authentication)
      : PubSubSession(unsolicited_output_bytes),
        transaction(transaction_buffer_bytes),
        connection_id(assigned_connection_id),
        default_authentication_required(require_authentication),
        authentication_required(require_authentication),
        authenticated(!require_authentication) {}

  detail::TransactionState transaction;
  resp::Version resp_version{resp::Version::resp2};
  std::uint64_t connection_id{0};
  bool default_authentication_required{false};
  bool authentication_required{false};
  bool authenticated{true};
  std::string authenticated_username;
  std::string client_name;
  std::string client_library_name;
  std::string client_library_version;
  bool quit_after_write{false};
  RespParser parser;
  std::string inbuf;   // raw accumulator: pre-decision bytes, then the SBE stream
  std::string output;
  std::vector<std::string_view> fields;
  std::size_t output_offset{0};
  std::vector<ReplyBoundary> replies;
  std::size_t reply_index{0};
  std::optional<BlockedListRequest> blocked_list;
  bool firehose{false};
  std::unique_ptr<detail::UnsolicitedOutputQueue> firehose_output;

  void record_reply(std::size_t prior_size) {
    if (output.size() != prior_size) {
      replies.push_back(ReplyBoundary{.sequence = next_output_sequence++,
                                      .end_offset = output.size()});
    }
  }

  void reset_connection(std::uint64_t assigned_connection_id) {
    wire_mode = detail::WireMode::undecided;
    resp_version = resp::Version::resp2;
    connection_id = assigned_connection_id;
    authentication_required = default_authentication_required;
    authenticated = !authentication_required;
    authenticated_username.clear();
    client_name.clear();
    client_library_name.clear();
    client_library_version.clear();
    quit_after_write = false;
    parser.clear();
    inbuf.clear();
    output.clear();
    output_offset = 0;
    replies.clear();
    reply_index = 0;
    unsolicited.clear();
    clear_unsolicited_front_cache();
    next_output_sequence = 1;
    close_requested = false;
    blocked_list.reset();
    firehose = false;
    firehose_output.reset();
    transaction.reset();
  }
};

class ReplicationRuntime {
 public:
  using SubscriberSet = ankerl::unordered_dense::set<EndpointSession*>;

#ifdef GOBLIN_HAS_KAFKA
  ReplicationRuntime(KafkaJournal* journal, std::size_t buffer_bytes)
      : buffer_bytes_(buffer_bytes), journal_(journal) {}
#else
  explicit ReplicationRuntime(std::size_t buffer_bytes)
      : buffer_bytes_(buffer_bytes) {}
#endif

  class Scope {
   public:
    Scope(ReplicationRuntime& runtime, Store& store) noexcept
        : runtime_(runtime), store_(store) {
      runtime_.begin();
    }
    ~Scope() noexcept { runtime_.finish(store_); }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

   private:
    ReplicationRuntime& runtime_;
    Store& store_;
  };

  [[nodiscard]] bool subscribe(EndpointSession& session, Store& store,
                               std::string& error) {
    remove(session);
    try {
      session.firehose_output =
          std::make_unique<detail::UnsolicitedOutputQueue>(buffer_bytes_);
    } catch (const std::bad_alloc&) {
      error = "ERR unable to map the replication output buffer";
      return false;
    }
    session.firehose = true;
    session.blocked_list.reset();
    session.transaction.reset();
    const auto hello = encode_firehose_hello(store.replication_state());
    session.output.append(hello);
    subscribers_.insert(&session);
    return true;
  }

  void remove(EndpointSession& session) noexcept {
    if (session.firehose) {
      (void)subscribers_.erase(&session);
      session.firehose = false;
      session.clear_unsolicited_front_cache();
      session.firehose_output.reset();
    }
  }

  void begin() noexcept { ++batch_depth_; }

  void finish(Store& store) noexcept {
    if (batch_depth_ == 0 || --batch_depth_ != 0 || pending_.empty() || failed_) {
      return;
    }
    commit(store);
  }

  static void replicate(void* context, Store& store, const Command& command,
                        std::string_view response) noexcept {
    auto& self = *static_cast<ReplicationRuntime*>(context);
    if (self.failed_) return;
    try {
      auto mutations = build_replication_mutations(store, command, response);
      if (self.batch_depth_ == 0) {
        self.pending_ = std::move(mutations);
        self.commit(store);
      } else {
        self.pending_.insert(self.pending_.end(),
                             std::make_move_iterator(mutations.begin()),
                             std::make_move_iterator(mutations.end()));
      }
    } catch (const std::exception& exception) {
      self.fail(exception.what());
    } catch (...) {
      self.fail("unknown replication serialization failure");
    }
  }

  [[nodiscard]] bool failed() const noexcept { return failed_; }
  [[nodiscard]] const std::string& error() const noexcept { return error_; }
  [[nodiscard]] std::size_t subscriber_count() const noexcept {
    return subscribers_.size();
  }

  [[nodiscard]] bool apply_received(Store& store,
                                    const ReplicationBatch& batch,
                                    std::string& error) {
    const auto prior_offset = store.replication_state().offset;
    if (!apply_firehose_batch(store, batch, error)) return false;
    if (store.replication_state().offset > prior_offset) broadcast(batch);
    return true;
  }

 private:
  void fail(std::string_view message) noexcept {
    if (failed_) return;
    failed_ = true;
    try {
      error_.assign(message);
    } catch (...) {
    }
  }

  void commit(Store& store) noexcept {
    if (pending_.empty() || failed_) return;
    try {
      auto state = store.replication_state();
      if (!state.valid || state.id.empty()) {
        store.reset_replication_identity();
        state = store.replication_state();
      }
      if (pending_.size() >
          std::numeric_limits<std::uint64_t>::max() - state.offset) {
        throw std::overflow_error("replication offset exhausted");
      }

      ReplicationBatch batch{.id = state.id,
                             .offset = state.offset + 1,
                             .mutations = std::move(pending_)};
      pending_.clear();
      auto offset = state.offset;
      for (const auto& mutation : batch.mutations) {
        ++offset;
#ifdef GOBLIN_HAS_KAFKA
        if (journal_ != nullptr &&
            !journal_->publish(batch.id, offset, mutation, error_)) {
          failed_ = true;
          return;
        }
#else
        (void)mutation;
#endif
        store.set_replication_offset(offset);
      }

      broadcast(batch);
    } catch (const std::exception& exception) {
      pending_.clear();
      fail(exception.what());
    } catch (...) {
      pending_.clear();
      fail("unknown replication commit failure");
    }
  }

  void broadcast(const ReplicationBatch& batch) noexcept {
    if (subscribers_.empty()) return;
    try {
      const auto wire = encode_firehose_batch(batch);
      for (auto* subscriber : subscribers_) {
        if (subscriber->firehose_output == nullptr ||
            !subscriber->firehose_output->push(
                subscriber->next_output_sequence, wire)) {
          subscriber->close_requested = true;
          overflowed_.push_back(subscriber);
          continue;
        }
        ++subscriber->next_output_sequence;
      }
      for (auto* subscriber : overflowed_) {
        remove(*subscriber);
      }
      overflowed_.clear();
    } catch (const std::exception& exception) {
      fail(exception.what());
    } catch (...) {
      fail("unknown firehose broadcast failure");
    }
  }

  SubscriberSet subscribers_;
  std::vector<EndpointSession*> overflowed_;
  std::vector<ReplicationMutation> pending_;
  std::size_t batch_depth_{0};
  std::size_t buffer_bytes_{0};
#ifdef GOBLIN_HAS_KAFKA
  KafkaJournal* journal_{nullptr};
#endif
  bool failed_{false};
  std::string error_;
};

class BlockingListRegistry {
 public:
  [[nodiscard]] bool park(EndpointSession& session, const Command& command) {
    if (session.blocked_list) {
      return true;
    }

    BlockedListRequest request;
    try {
      request.fields.reserve(command.args.size() + 1);
      request.fields.emplace_back(command.name);
      for (const auto arg : command.args) {
        request.fields.emplace_back(arg);
      }
      request.null_array = null_array_reply(command.type);
      if (command.list_timeout_seconds > 0.0) {
        const auto now = BlockedListRequest::Clock::now();
        const auto available = BlockedListRequest::Clock::time_point::max() - now;
        const auto available_seconds =
            std::chrono::duration<double>(available).count();
        request.deadline = command.list_timeout_seconds >= available_seconds
                               ? BlockedListRequest::Clock::time_point::max()
                               : now + std::chrono::duration_cast<
                                           BlockedListRequest::Clock::duration>(
                                           std::chrono::duration<double>(
                                               command.list_timeout_seconds));
      }
      session.blocked_list.emplace(std::move(request));
      waiters_.push_back(&session);
      return true;
    } catch (const std::bad_alloc&) {
      session.blocked_list.reset();
      return false;
    }
  }

  void remove(EndpointSession& session) noexcept {
    const auto found = std::find(waiters_.begin(), waiters_.end(), &session);
    if (found != waiters_.end()) {
      waiters_.erase(found);
    }
    session.blocked_list.reset();
  }

  void modified() noexcept { dirty_ = true; }

  void serve_ready(Store& store, ReplicationRuntime& replication) {
    if (!dirty_ && !deadline_due(BlockedListRequest::Clock::now())) {
      return;
    }
    dirty_ = false;

    std::size_t index = 0;
    while (index < waiters_.size()) {
      auto& session = *waiters_[index];
      if (!session.blocked_list) {
        waiters_.erase(waiters_.begin() + static_cast<std::ptrdiff_t>(index));
        continue;
      }

      auto& request = *session.blocked_list;
      session.fields.clear();
      session.fields.reserve(request.fields.size());
      for (const auto& field : request.fields) {
        session.fields.emplace_back(field);
      }

      const auto parsed = parse_command(session.fields);
      const std::size_t prior_size = session.output.size();
      ReplicationRuntime::Scope replication_scope(replication, store);
      if (!parsed.ok()) {
        resp::append_error(session.output, parsed.error);
      } else {
        CommandExecutionOptions options;
        options.resp_version = &session.resp_version;
        options.replication_context = &replication;
        options.replicate_write = &ReplicationRuntime::replicate;
        options.blocking_lists = BlockingListDispatch{
            .context = nullptr,
            .park = [](void*, const Command&) { return true; }};
        execute_command_into(store, *parsed.command, session.output, options);
      }

      if (session.output.size() != prior_size) {
        session.record_reply(prior_size);
        session.fields.clear();
        session.blocked_list.reset();
        waiters_.erase(waiters_.begin() + static_cast<std::ptrdiff_t>(index));
        // A successful BLMOVE can make a destination ready for an older waiter.
        index = 0;
        continue;
      }

      if (request.deadline &&
          *request.deadline <= BlockedListRequest::Clock::now()) {
        if (request.null_array && session.resp_version == resp::Version::resp2) {
          session.output.append("*-1\r\n");
        } else {
          resp::append_null(session.output, session.resp_version);
        }
        session.record_reply(prior_size);
        session.fields.clear();
        session.blocked_list.reset();
        waiters_.erase(waiters_.begin() + static_cast<std::ptrdiff_t>(index));
        continue;
      }
      session.fields.clear();
      ++index;
    }
    dirty_ = false;
  }

  [[nodiscard]] int clamp_poll_timeout(int requested_ms) const noexcept {
    if (requested_ms <= 0) {
      return requested_ms;
    }
    const auto now = BlockedListRequest::Clock::now();
    int result = requested_ms;
    for (const auto* session : waiters_) {
      if (session == nullptr || !session->blocked_list ||
          !session->blocked_list->deadline) {
        continue;
      }
      const auto deadline = *session->blocked_list->deadline;
      if (deadline <= now) {
        return 0;
      }
      const auto remaining = deadline - now;
      auto rounded =
          std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
      if (std::chrono::duration_cast<BlockedListRequest::Clock::duration>(
              rounded) < remaining) {
        rounded += std::chrono::milliseconds(1);
      }
      const auto millis = rounded.count();
      result = std::min(result, static_cast<int>(std::min<long long>(
                                    millis, std::numeric_limits<int>::max())));
    }
    return result;
  }

 private:
  [[nodiscard]] static bool null_array_reply(CommandType type) noexcept {
    switch (type) {
      case CommandType::blpop:
      case CommandType::brpop:
      case CommandType::blmpop:
      case CommandType::pma_blpop:
      case CommandType::pma_brpop:
      case CommandType::pma_blmpop:
      case CommandType::segmented_blpop:
      case CommandType::segmented_brpop:
      case CommandType::segmented_blmpop:
        return true;
      default:
        return false;
    }
  }

  [[nodiscard]] bool deadline_due(
      BlockedListRequest::Clock::time_point now) const noexcept {
    return std::any_of(waiters_.begin(), waiters_.end(), [now](const auto* waiter) {
      return waiter != nullptr && waiter->blocked_list &&
             waiter->blocked_list->deadline &&
             *waiter->blocked_list->deadline <= now;
    });
  }

  std::vector<EndpointSession*> waiters_;
  bool dirty_{false};
};

struct BlockingListParkContext {
  BlockingListRegistry* registry{nullptr};
  EndpointSession* session{nullptr};
};

[[nodiscard]] bool park_blocking_list(void* context, const Command& command) {
  auto& park = *static_cast<BlockingListParkContext*>(context);
  return park.registry->park(*park.session, command);
}

struct Client : EndpointSession {
  Client(int client_fd, std::size_t unsolicited_output_bytes,
         std::size_t transaction_buffer_bytes,
         std::uint64_t assigned_connection_id, bool require_authentication)
      : EndpointSession(unsolicited_output_bytes, transaction_buffer_bytes,
                        assigned_connection_id, require_authentication),
        fd(client_fd) {}

  ~Client() {
#ifdef GOBLIN_HAS_TLS
    if (tls != nullptr) {
      SSL_free(tls);
    }
#endif
  }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;

#ifdef GOBLIN_HAS_TLS
  [[nodiscard]] bool enable_tls(SSL_CTX* context) {
    SSL* connection = SSL_new(context);
    if (connection == nullptr) {
      return false;
    }
    if (SSL_set_fd(connection, fd) != 1) {
      SSL_free(connection);
      return false;
    }
    SSL_set_accept_state(connection);
    tls = connection;
    tls_handshake_complete = false;
    tls_handshake_want = TlsWant::Read;
    return true;
  }
#endif

  int fd{-1};
  bool read_backpressured{false};
  bool close_after_write{false};
#ifdef GOBLIN_HAS_TLS
  SSL* tls{nullptr};
  bool tls_handshake_complete{false};
  TlsWant tls_handshake_want{TlsWant::Read};
  TlsWant tls_read_want{TlsWant::Read};
  TlsWant tls_write_want{TlsWant::Write};
  std::size_t tls_write_retry_bytes{0};
  bool tls_write_retry_unsolicited{false};
#endif
};

// Server-side state for one shared-memory ring: the mapping plus a RESP parser
// and reply buffer, mirroring a network Client but reading from the SQ and writing
// to the CQ instead of a socket.
struct RingEndpoint : EndpointSession {
  RingEndpoint(ring::Mapping&& ring_mapping,
               std::size_t unsolicited_output_bytes,
               std::size_t transaction_buffer_bytes,
               std::uint64_t assigned_connection_id,
               bool require_authentication)
      : EndpointSession(unsolicited_output_bytes, transaction_buffer_bytes,
                        assigned_connection_id, require_authentication),
        mapping(std::move(ring_mapping)),
        sq(mapping.sq_consumer()),
        cq(mapping.cq_producer()) {}

  ring::Mapping mapping;
  // Long-lived views: Producer's cached_head_ only pays off if the object
  // survives across replies (recreating cq_producer() every record re-seeds it).
  // Rebind both after drain_for_reconnect so local cursors match the new indices.
  ring::Consumer sq;
  ring::Producer cq;
  // Local mirror of mapping.acked_epoch(); avoids a second shared-memory load on
  // every empty poll of the reconnect check.
  std::uint64_t acked_epoch{0};

  void rebind_ring_views() noexcept {
    sq = mapping.sq_consumer();
    cq = mapping.cq_producer();
  }
};

#ifdef GOBLIN_HAS_RDMA
struct RdmaEndpoint : EndpointSession {
  RdmaEndpoint(std::unique_ptr<rdma::Connection> rdma_connection,
               std::size_t unsolicited_output_bytes,
               std::size_t transaction_buffer_bytes,
               std::uint64_t assigned_connection_id,
               bool require_authentication)
      : EndpointSession(unsolicited_output_bytes, transaction_buffer_bytes,
                        assigned_connection_id, require_authentication),
        connection(std::move(rdma_connection)) {}

  std::unique_ptr<rdma::Connection> connection;
  bool disconnect_started{false};
};

// The listener is declared before the endpoints so destruction runs in the
// reverse order: every QP/CM id is torn down before its shared event channel.
struct RdmaRuntimeTarget {
  std::unique_ptr<rdma::ServerListener> listener;
  std::vector<std::unique_ptr<RdmaEndpoint>> endpoints;
  std::size_t next_endpoint{0};
  bool listener_error_reported{false};
};
#endif

#ifdef GOBLIN_HAS_EXASOCK
// Priority TCP listener for `--exasock`. Clients are ordinary non-blocking
// sockets; under the exasock wrapper + ExaNIC bind they are accelerated.
struct ExasockRuntimeTarget {
  int listener_fd{-1};
  std::vector<std::unique_ptr<Client>> clients;
  std::size_t next_client{0};
};
#endif

struct PolledRuntimeTarget {
  std::unique_ptr<RingEndpoint> ring_endpoint;
#ifdef GOBLIN_HAS_RDMA
  std::unique_ptr<RdmaRuntimeTarget> rdma_target;
#endif
#ifdef GOBLIN_HAS_EXASOCK
  std::unique_ptr<ExasockRuntimeTarget> exasock_target;
#endif
};

[[nodiscard]] std::uint64_t next_connection_id() noexcept {
  static std::uint64_t next = 1;
  return next++;
}

void close_fd(int fd) noexcept {
  if (fd >= 0) {
    (void)::close(fd);
  }
}

[[nodiscard]] bool would_block() noexcept {
  return errno == EAGAIN || errno == EWOULDBLOCK;
}

#ifdef GOBLIN_HAS_TLS
using TlsContextPtr = std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)>;

[[nodiscard]] std::string tls_error_text(std::string_view operation) {
  std::string result(operation);
  unsigned long code = 0;
  unsigned long last = 0;
  while ((code = ERR_get_error()) != 0) {
    last = code;
  }
  if (last != 0) {
    char detail[256];
    ERR_error_string_n(last, detail, sizeof(detail));
    result.append(": ");
    result.append(detail);
  }
  return result;
}

[[nodiscard]] TlsContextPtr create_tls_context(const TlsConfig& config,
                                               std::string& error) {
  ERR_clear_error();
  SSL_CTX* raw = SSL_CTX_new(TLS_server_method());
  TlsContextPtr context(raw, &SSL_CTX_free);
  if (!context) {
    error = tls_error_text("could not create TLS server context");
    return context;
  }
  if (SSL_CTX_set_min_proto_version(context.get(), TLS1_2_VERSION) != 1) {
    error = tls_error_text("could not require TLS 1.2 or newer");
    return TlsContextPtr(nullptr, &SSL_CTX_free);
  }
  long tls_options = SSL_OP_NO_COMPRESSION;
#ifdef SSL_OP_NO_RENEGOTIATION
  tls_options |= SSL_OP_NO_RENEGOTIATION;
#endif
  SSL_CTX_set_options(context.get(), tls_options);
  SSL_CTX_set_session_cache_mode(context.get(), SSL_SESS_CACHE_OFF);
  SSL_CTX_set_mode(context.get(),
                   SSL_MODE_ENABLE_PARTIAL_WRITE |
                       SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER |
                       SSL_MODE_RELEASE_BUFFERS);
  ERR_clear_error();
  if (SSL_CTX_use_certificate_chain_file(
          context.get(), config.certificate_chain_file.c_str()) != 1) {
    error = tls_error_text("could not load TLS certificate chain");
    return TlsContextPtr(nullptr, &SSL_CTX_free);
  }
  ERR_clear_error();
  if (SSL_CTX_use_PrivateKey_file(context.get(), config.private_key_file.c_str(),
                                  SSL_FILETYPE_PEM) != 1) {
    error = tls_error_text("could not load TLS private key");
    return TlsContextPtr(nullptr, &SSL_CTX_free);
  }
  ERR_clear_error();
  if (SSL_CTX_check_private_key(context.get()) != 1) {
    error = tls_error_text("TLS private key does not match the certificate");
    return TlsContextPtr(nullptr, &SSL_CTX_free);
  }
  return context;
}
#endif

enum class TransportIoStatus : std::uint8_t {
  Progress,
  WouldBlock,
  Interrupted,
  Closed,
  Error,
};

struct TransportIoResult {
  TransportIoStatus status{TransportIoStatus::Error};
  std::size_t bytes{0};
};

[[nodiscard]] TransportIoResult transport_read(Client& client, char* buffer,
                                               std::size_t size) {
#ifdef GOBLIN_HAS_TLS
  if (client.tls != nullptr) {
    std::size_t received = 0;
    ERR_clear_error();
    errno = 0;
    const int result = SSL_read_ex(client.tls, buffer, size, &received);
    if (result == 1) {
      client.tls_read_want = TlsWant::Read;
      return {.status = TransportIoStatus::Progress, .bytes = received};
    }
    switch (SSL_get_error(client.tls, result)) {
      case SSL_ERROR_WANT_READ:
        client.tls_read_want = TlsWant::Read;
        return {.status = TransportIoStatus::WouldBlock};
      case SSL_ERROR_WANT_WRITE:
        client.tls_read_want = TlsWant::Write;
        return {.status = TransportIoStatus::WouldBlock};
      case SSL_ERROR_ZERO_RETURN:
        return {.status = TransportIoStatus::Closed};
      case SSL_ERROR_SYSCALL:
        if (errno == EINTR) {
          return {.status = TransportIoStatus::Interrupted};
        }
        if (would_block()) {
          client.tls_read_want = TlsWant::Read;
          return {.status = TransportIoStatus::WouldBlock};
        }
        return {.status = TransportIoStatus::Error};
      default:
        return {.status = TransportIoStatus::Error};
    }
  }
#endif
  const auto received = ::recv(client.fd, buffer, size, 0);
  if (received > 0) {
    return {.status = TransportIoStatus::Progress,
            .bytes = static_cast<std::size_t>(received)};
  }
  if (received == 0) {
    return {.status = TransportIoStatus::Closed};
  }
  if (would_block()) {
    return {.status = TransportIoStatus::WouldBlock};
  }
  if (errno == EINTR) {
    return {.status = TransportIoStatus::Interrupted};
  }
  return {.status = TransportIoStatus::Error};
}

[[nodiscard]] TransportIoResult transport_write(Client& client,
                                                const char* bytes,
                                                std::size_t size,
                                                bool unsolicited) {
#ifdef GOBLIN_HAS_TLS
  if (client.tls != nullptr) {
    std::size_t sent = 0;
    ERR_clear_error();
    errno = 0;
    const int result = SSL_write_ex(client.tls, bytes, size, &sent);
    if (result == 1) {
      client.tls_write_want = TlsWant::Write;
      client.tls_write_retry_bytes = 0;
      return {.status = TransportIoStatus::Progress, .bytes = sent};
    }
    const auto remember_retry = [&] {
      if (client.tls_write_retry_bytes == 0) {
        client.tls_write_retry_bytes = size;
        client.tls_write_retry_unsolicited = unsolicited;
      }
    };
    switch (SSL_get_error(client.tls, result)) {
      case SSL_ERROR_WANT_READ:
        remember_retry();
        client.tls_write_want = TlsWant::Read;
        return {.status = TransportIoStatus::WouldBlock};
      case SSL_ERROR_WANT_WRITE:
        remember_retry();
        client.tls_write_want = TlsWant::Write;
        return {.status = TransportIoStatus::WouldBlock};
      case SSL_ERROR_ZERO_RETURN:
        return {.status = TransportIoStatus::Closed};
      case SSL_ERROR_SYSCALL:
        if (errno == EINTR) {
          remember_retry();
          return {.status = TransportIoStatus::Interrupted};
        }
        if (would_block()) {
          remember_retry();
          client.tls_write_want = TlsWant::Write;
          return {.status = TransportIoStatus::WouldBlock};
        }
        return {.status = TransportIoStatus::Error};
      default:
        return {.status = TransportIoStatus::Error};
    }
  }
#endif
  (void)unsolicited;
  const auto sent = ::send(client.fd, bytes, size, 0);
  if (sent > 0) {
    return {.status = TransportIoStatus::Progress,
            .bytes = static_cast<std::size_t>(sent)};
  }
  if (sent < 0 && would_block()) {
    return {.status = TransportIoStatus::WouldBlock};
  }
  if (sent < 0 && errno == EINTR) {
    return {.status = TransportIoStatus::Interrupted};
  }
  return {.status = sent == 0 ? TransportIoStatus::Closed
                              : TransportIoStatus::Error};
}

[[nodiscard]] bool tls_handshake_pending(const Client& client) noexcept {
#ifdef GOBLIN_HAS_TLS
  return client.tls != nullptr && !client.tls_handshake_complete;
#else
  (void)client;
  return false;
#endif
}

[[nodiscard]] bool tls_write_retry_pending(const Client& client) noexcept {
#ifdef GOBLIN_HAS_TLS
  return client.tls != nullptr && client.tls_write_retry_bytes != 0;
#else
  (void)client;
  return false;
#endif
}

[[nodiscard]] bool tls_read_retry_pending(const Client& client) noexcept {
#ifdef GOBLIN_HAS_TLS
  return client.tls != nullptr && client.tls_handshake_complete &&
         client.tls_read_want == TlsWant::Write;
#else
  (void)client;
  return false;
#endif
}

[[nodiscard]] bool tls_plaintext_pending(const Client& client) noexcept {
#ifdef GOBLIN_HAS_TLS
  return client.tls != nullptr && client.tls_handshake_complete &&
         !tls_write_retry_pending(client) && SSL_pending(client.tls) > 0;
#else
  (void)client;
  return false;
#endif
}

[[nodiscard]] bool has_pending_output(
    const EndpointSession& session) noexcept;

#ifdef GOBLIN_HAS_TLS
[[nodiscard]] short tls_want_event(TlsWant want) noexcept {
  return want == TlsWant::Read ? POLLIN : POLLOUT;
}
#endif

[[nodiscard]] short client_poll_events(const Client& client) noexcept {
#ifdef GOBLIN_HAS_TLS
  if (tls_handshake_pending(client)) {
    return tls_want_event(client.tls_handshake_want);
  }
  if (tls_write_retry_pending(client)) {
    return tls_want_event(client.tls_write_want);
  }
  if (tls_read_retry_pending(client)) {
    return POLLOUT;
  }
#endif
  short events = 0;
  if (!client.firehose && !client.read_backpressured) {
#ifdef GOBLIN_HAS_TLS
    events |= client.tls != nullptr ? tls_want_event(client.tls_read_want)
                                    : POLLIN;
#else
    events |= POLLIN;
#endif
  }
  if (has_pending_output(client)) {
#ifdef GOBLIN_HAS_TLS
    events |= client.tls != nullptr ? tls_want_event(client.tls_write_want)
                                    : POLLOUT;
#else
    events |= POLLOUT;
#endif
  }
  return events;
}

[[nodiscard]] bool client_read_ready(const Client& client,
                                     short revents) noexcept {
  if (tls_write_retry_pending(client)) {
    return false;
  }
  if (tls_plaintext_pending(client)) {
    return true;
  }
#ifdef GOBLIN_HAS_TLS
  if (client.tls != nullptr) {
    return (revents & tls_want_event(client.tls_read_want)) != 0;
  }
#endif
  return (revents & POLLIN) != 0;
}

[[nodiscard]] bool client_write_ready(const Client& client,
                                      short revents) noexcept {
  if (tls_read_retry_pending(client)) {
    return false;
  }
#ifdef GOBLIN_HAS_TLS
  if (client.tls != nullptr) {
    return (revents & tls_want_event(client.tls_write_want)) != 0;
  }
#else
  (void)client;
#endif
  return (revents & POLLOUT) != 0;
}

[[nodiscard]] bool tls_handshake_ready(const Client& client,
                                       short revents) noexcept {
#ifdef GOBLIN_HAS_TLS
  return tls_handshake_pending(client) &&
         (revents & tls_want_event(client.tls_handshake_want)) != 0;
#else
  (void)client;
  (void)revents;
  return false;
#endif
}

[[nodiscard]] bool advance_tls_handshake(Client& client) {
#ifdef GOBLIN_HAS_TLS
  if (!tls_handshake_pending(client)) {
    return true;
  }
  ERR_clear_error();
  errno = 0;
  const int result = SSL_accept(client.tls);
  if (result == 1) {
    client.tls_handshake_complete = true;
    client.tls_read_want = TlsWant::Read;
    client.tls_write_want = TlsWant::Write;
    return true;
  }
  switch (SSL_get_error(client.tls, result)) {
    case SSL_ERROR_WANT_READ:
      client.tls_handshake_want = TlsWant::Read;
      return true;
    case SSL_ERROR_WANT_WRITE:
      client.tls_handshake_want = TlsWant::Write;
      return true;
    default:
      return false;
  }
#else
  (void)client;
  return true;
#endif
}

[[nodiscard]] bool has_pending_regular_output(
    const EndpointSession& session) noexcept {
  return session.reply_index < session.replies.size();
}

[[nodiscard]] const detail::UnsolicitedOutputQueue& unsolicited_queue(
    const EndpointSession& session) noexcept {
  return session.firehose_output ? *session.firehose_output
                                 : session.unsolicited;
}

[[nodiscard]] detail::UnsolicitedOutputQueue& unsolicited_queue(
    EndpointSession& session) noexcept {
  return session.firehose_output ? *session.firehose_output
                                 : session.unsolicited;
}

[[nodiscard]] bool has_pending_output(const EndpointSession& session) noexcept {
  return has_pending_regular_output(session) ||
         !unsolicited_queue(session).empty();
}

// Peek the unsolicited head, caching the mmap view across partial writes.
[[nodiscard]] bool peek_unsolicited(EndpointSession& session,
                                    detail::UnsolicitedOutputQueue::Front& out) noexcept {
  if (session.has_unsolicited_front) {
    out = session.unsolicited_front;
    return true;
  }
  auto front = unsolicited_queue(session).front();
  if (!front) {
    return false;
  }
  session.unsolicited_front = *front;
  session.has_unsolicited_front = true;
  out = session.unsolicited_front;
  return true;
}

void consume_unsolicited_front(EndpointSession& session) noexcept {
  const std::size_t payload_len = session.unsolicited_front.bytes.size();
  unsolicited_queue(session).pop_front(payload_len);
  session.clear_unsolicited_front_cache();
}

[[nodiscard]] std::size_t pending_output_bytes(
    const EndpointSession& session) noexcept {
  const std::size_t regular = session.output.size() - session.output_offset;
  const std::size_t unsolicited =
      unsolicited_queue(session).payload_bytes() >=
              session.unsolicited_front_offset
          ? unsolicited_queue(session).payload_bytes() -
                session.unsolicited_front_offset
          : 0;
  return regular + unsolicited;
}

void update_read_backpressure(Client& client, const ServerConfig& config) {
  if (config.max_output_buffer_bytes == 0) {
    client.read_backpressured = false;
    return;
  }

  const auto pending = pending_output_bytes(client);
  if (client.read_backpressured) {
    if (pending <= config.resume_output_buffer_bytes) {
      client.read_backpressured = false;
    }
    return;
  }

  if (pending >= config.max_output_buffer_bytes) {
    client.read_backpressured = true;
  }
}

void compact_output_if_needed(EndpointSession& session) {
  if (session.output_offset == 0) {
    return;
  }

  if (session.output_offset >= session.output.size()) {
    session.output.clear();
    session.output_offset = 0;
    session.replies.clear();
    session.reply_index = 0;
    return;
  }

  const auto remaining = session.output.size() - session.output_offset;
  if (session.output_offset >= kOutputCompactThreshold &&
      session.output_offset >= remaining) {
    const auto removed = session.output_offset;
    session.output.erase(0, removed);
    for (std::size_t i = session.reply_index; i < session.replies.size(); ++i) {
      session.replies[i].end_offset -= removed;
    }
    if (session.reply_index != 0) {
      session.replies.erase(session.replies.begin(),
                            session.replies.begin() +
                                static_cast<std::ptrdiff_t>(session.reply_index));
      session.reply_index = 0;
    }
    session.output_offset = 0;
  }
}

[[nodiscard]] bool set_nonblocking(int fd) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return false;
  }
  return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

void set_no_sigpipe(int fd) {
#ifdef SO_NOSIGPIPE
  int enabled = 1;
  (void)::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#else
  (void)fd;
#endif
}

void set_tcp_nodelay(int fd) {
  int enabled = 1;
  if (::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled)) != 0) {
    std::cerr << "goblin-core: setsockopt(TCP_NODELAY) failed: " << std::strerror(errno)
              << '\n';
  }
}

struct SocketListenerRuntime {
  int fd{-1};
  bool tcp{false};
  bool tls{false};
  std::string description;
  std::string uds_path;
};

[[nodiscard]] std::string format_tcp_endpoint(std::string_view address,
                                              std::uint16_t port) {
  std::string result;
  if (address.find(':') != std::string_view::npos) {
    result.push_back('[');
    result.append(address);
    result.push_back(']');
  } else {
    result.assign(address);
  }
  result.push_back(':');
  result.append(std::to_string(port));
  return result;
}

[[nodiscard]] bool is_loopback_address(std::string_view address) {
  const std::string text(address);
  in_addr ipv4{};
  if (::inet_pton(AF_INET, text.c_str(), &ipv4) == 1) {
    return (ntohl(ipv4.s_addr) & 0xff000000U) == 0x7f000000U;
  }
  in6_addr ipv6{};
  return ::inet_pton(AF_INET6, text.c_str(), &ipv6) == 1 &&
         IN6_IS_ADDR_LOOPBACK(&ipv6);
}

void normalize_socket_listeners(ServerConfig& config) {
  if (config.socket_listeners.empty() && !config.unix_socket_path.empty()) {
    config.socket_listeners.emplace_back(
        UdsListenerConfig{.path = config.unix_socket_path});
  }
  if (config.socket_listeners.empty() &&
      config.bind_address != "127.0.0.1") {
    config.socket_listeners.emplace_back(TcpListenerConfig{
        .bind_address = config.bind_address,
        .port = config.port,
        .tls = !is_loopback_address(config.bind_address)});
  }

  std::vector<std::uint16_t> tcp_ports;
  for (auto& configured : config.socket_listeners) {
    if (auto* tcp = std::get_if<TcpListenerConfig>(&configured)) {
      tcp->tls = tcp->tls || !is_loopback_address(tcp->bind_address);
      if (std::find(tcp_ports.begin(), tcp_ports.end(), tcp->port) ==
          tcp_ports.end()) {
        tcp_ports.push_back(tcp->port);
      }
    }
  }
  if (tcp_ports.empty()) {
    tcp_ports.push_back(config.port);
  }
  for (const auto port : tcp_ports) {
    const bool already_present = std::any_of(
        config.socket_listeners.begin(), config.socket_listeners.end(),
        [port](const auto& configured) {
          const auto* tcp = std::get_if<TcpListenerConfig>(&configured);
          return tcp != nullptr && tcp->port == port &&
                 tcp->bind_address == "127.0.0.1" && !tcp->tls;
        });
    if (!already_present) {
      config.socket_listeners.emplace_back(TcpListenerConfig{
          .bind_address = "127.0.0.1", .port = port, .tls = false});
    }
  }
}

[[nodiscard]] std::optional<SocketListenerRuntime> create_unix_listener(
    const UdsListenerConfig& config, int backlog) {
  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "goblin-core: socket(AF_UNIX) failed: " << std::strerror(errno)
              << '\n';
    return std::nullopt;
  }
  set_no_sigpipe(fd);

  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  if (config.path.size() >= sizeof(address.sun_path)) {
    std::cerr << "goblin-core: unix socket path too long: " << config.path
              << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  std::memcpy(address.sun_path, config.path.c_str(), config.path.size() + 1);
  ::unlink(config.path.c_str());  // clear any stale socket file

  if (::bind(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
    std::cerr << "goblin-core: bind(unix " << config.path
              << ") failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (::listen(fd, backlog) != 0) {
    std::cerr << "goblin-core: listen failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    ::unlink(config.path.c_str());
    return std::nullopt;
  }
  if (!set_nonblocking(fd)) {
    std::cerr << "goblin-core: failed to set listener nonblocking: "
              << std::strerror(errno) << '\n';
    close_fd(fd);
    ::unlink(config.path.c_str());
    return std::nullopt;
  }
  return SocketListenerRuntime{.fd = fd,
                               .tcp = false,
                               .tls = false,
                               .description = "unix:" + config.path,
                               .uds_path = config.path};
}

[[nodiscard]] std::optional<SocketListenerRuntime> create_network_listener(
    const TcpListenerConfig& config, int backlog) {
  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;
  hints.ai_flags = AI_NUMERICHOST | AI_NUMERICSERV;

  addrinfo* resolved = nullptr;
  const std::string port_text = std::to_string(config.port);
  const int lookup = ::getaddrinfo(config.bind_address.c_str(),
                                   port_text.c_str(), &hints, &resolved);
  if (lookup != 0) {
    std::cerr << "goblin-core: invalid TCP bind address "
              << format_tcp_endpoint(config.bind_address, config.port) << ": "
              << ::gai_strerror(lookup) << '\n';
    return std::nullopt;
  }

  int last_error = 0;
  for (const addrinfo* candidate = resolved; candidate != nullptr;
       candidate = candidate->ai_next) {
    const int fd = ::socket(candidate->ai_family, candidate->ai_socktype,
                            candidate->ai_protocol);
    if (fd < 0) {
      last_error = errno;
      continue;
    }

    int enabled = 1;
    if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enabled,
                     sizeof(enabled)) != 0) {
      last_error = errno;
      close_fd(fd);
      continue;
    }
    if (candidate->ai_family == AF_INET6 &&
        ::setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &enabled,
                     sizeof(enabled)) != 0) {
      last_error = errno;
      close_fd(fd);
      continue;
    }
    set_no_sigpipe(fd);

    if (::bind(fd, candidate->ai_addr,
               static_cast<socklen_t>(candidate->ai_addrlen)) != 0 ||
        ::listen(fd, backlog) != 0 || !set_nonblocking(fd)) {
      last_error = errno;
      close_fd(fd);
      continue;
    }

    ::freeaddrinfo(resolved);
    return SocketListenerRuntime{
        .fd = fd,
        .tcp = true,
        .tls = config.tls,
        .description =
            std::string(config.tls ? "tls://" : "tcp://") +
            format_tcp_endpoint(config.bind_address, config.port),
        .uds_path = {}};
  }

  ::freeaddrinfo(resolved);
  std::cerr << "goblin-core: bind/listen "
            << format_tcp_endpoint(config.bind_address, config.port)
            << " failed: " << std::strerror(last_error) << '\n';
  return std::nullopt;
}

void close_socket_listeners(
    std::vector<SocketListenerRuntime>& listeners) noexcept {
  for (auto& listener : listeners) {
    close_fd(listener.fd);
    listener.fd = -1;
    if (!listener.uds_path.empty()) {
      (void)::unlink(listener.uds_path.c_str());
    }
  }
}

[[nodiscard]] std::optional<std::vector<SocketListenerRuntime>>
create_socket_listeners(const ServerConfig& config) {
  std::vector<SocketListenerRuntime> listeners;
  listeners.reserve(config.socket_listeners.size());
  for (const auto& configured : config.socket_listeners) {
    std::optional<SocketListenerRuntime> listener;
    if (const auto* tcp = std::get_if<TcpListenerConfig>(&configured)) {
      listener = create_network_listener(*tcp, config.backlog);
    } else {
      listener = create_unix_listener(std::get<UdsListenerConfig>(configured),
                                      config.backlog);
    }
    if (!listener) {
      close_socket_listeners(listeners);
      return std::nullopt;
    }
    listeners.push_back(std::move(*listener));
  }
  return listeners;
}

void accept_clients(int listener_fd, bool tcp,
                    std::vector<std::unique_ptr<Client>>& clients,
                    const ServerConfig& config, void* tls_context = nullptr) {
  for (;;) {
    sockaddr_storage address{};
    socklen_t address_len = sizeof(address);
    const int client_fd =
        ::accept(listener_fd, reinterpret_cast<sockaddr*>(&address), &address_len);
    if (client_fd < 0) {
      if (would_block()) {
        return;
      }
      if (errno == EINTR) {
        continue;
      }
      std::cerr << "goblin-core: accept failed: " << std::strerror(errno) << '\n';
      return;
    }

    if (!set_nonblocking(client_fd)) {
      std::cerr << "goblin-core: failed to set client nonblocking: " << std::strerror(errno)
                << '\n';
      close_fd(client_fd);
      continue;
    }
    set_no_sigpipe(client_fd);
    if (tcp) {
      set_tcp_nodelay(client_fd);
    }

    try {
      auto client = std::make_unique<Client>(
          client_fd, config.unsolicited_output_buffer_bytes,
          config.transaction_buffer_bytes, next_connection_id(),
          config.auth_file.has_value());
#ifdef GOBLIN_HAS_TLS
      if (tls_context != nullptr &&
          !client->enable_tls(static_cast<SSL_CTX*>(tls_context))) {
        std::cerr << "goblin-core: unable to create TLS client state\n";
        close_fd(client_fd);
        continue;
      }
#else
      (void)tls_context;
#endif
      if (config.initial_output_buffer_bytes > 0) {
        client->output.reserve(config.initial_output_buffer_bytes);
      }
      clients.push_back(std::move(client));
    } catch (const std::bad_alloc&) {
      std::cerr << "goblin-core: unable to map client buffers\n";
      close_fd(client_fd);
    }
  }
}

[[nodiscard]] bool is_pubsub_command(CommandType type) noexcept {
  switch (type) {
    case CommandType::subscribe:
    case CommandType::unsubscribe:
    case CommandType::psubscribe:
    case CommandType::punsubscribe:
    case CommandType::publish:
    case CommandType::pubsub:
      return true;
    default:
      return false;
  }
}

[[nodiscard]] bool allowed_while_resp2_subscribed(CommandType type) noexcept {
  return type == CommandType::subscribe || type == CommandType::unsubscribe ||
         type == CommandType::psubscribe || type == CommandType::punsubscribe ||
         type == CommandType::ping || type == CommandType::quit;
}

[[nodiscard]] bool allowed_before_authentication(CommandType type) noexcept {
  return type == CommandType::auth || type == CommandType::hello ||
         type == CommandType::quit;
}

[[nodiscard]] bool equals_ascii_ci(std::string_view lhs,
                                   std::string_view rhs) noexcept {
  if (lhs.size() != rhs.size()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.size(); ++i) {
    const auto upper = [](unsigned char byte) {
      return byte >= 'a' && byte <= 'z'
                 ? static_cast<unsigned char>(byte - ('a' - 'A'))
                 : byte;
    };
    if (upper(static_cast<unsigned char>(lhs[i])) !=
        upper(static_cast<unsigned char>(rhs[i]))) {
      return false;
    }
  }
  return true;
}

void scrub_auth_request(std::span<const std::string_view> fields) noexcept {
  if (fields.empty() ||
      (!equals_ascii_ci(fields.front(), "AUTH") &&
       !equals_ascii_ci(fields.front(), "HELLO"))) {
    return;
  }
  for (const auto field : fields.subspan(1)) {
    secure_zero_memory(const_cast<char*>(field.data()), field.size());
  }
}

void append_transaction_limit_error(std::string& out,
                                    std::size_t mapped_bytes) {
  resp::append_error(
      out, "ERR transaction exceeds the configured " +
               std::to_string(mapped_bytes) + "-byte buffer limit");
}

void execute_queued_transaction(EndpointSession& session, Store& store,
                                detail::PubSubRegistry& pubsub,
                                detail::WatchRegistry& watches,
                                const CommandExecutionOptions& exec_options) {
  auto& transaction = session.transaction;
  if (!transaction.in_multi) {
    resp::append_error(session.output, "ERR EXEC without MULTI");
    return;
  }

  const bool watch_dirty = transaction.watch_dirty;
  watches.remove(transaction);
  if (watch_dirty) {
    transaction.finish();
    if (session.resp_version == resp::Version::resp3) {
      resp::append_null(session.output, session.resp_version);
    } else {
      session.output.append("*-1\r\n");
    }
    return;
  }

  const auto failure = transaction.failure;
  if (failure != detail::TransactionFailure::none) {
    const auto capacity = transaction.commands.mapped_bytes();
    transaction.finish();
    if (failure == detail::TransactionFailure::buffer_limit) {
      resp::append_error(
          session.output,
          "EXECABORT Transaction discarded because its " +
              std::to_string(capacity) + "-byte buffer limit was exceeded.");
    } else {
      resp::append_error(
          session.output,
          "EXECABORT Transaction discarded because of previous errors.");
    }
    return;
  }

  const std::size_t count = transaction.commands.command_count();
  resp::append_array_header(session.output, count);
  transaction.in_multi = false;
  auto queued_options = exec_options;
  queued_options.blocking_lists = {};
  std::size_t offset = 0;
  for (std::size_t index = 0; index < count; ++index) {
    if (!transaction.commands.decode(offset, session.fields)) {
      resp::append_error(session.output, "ERR corrupt transaction buffer");
      break;
    }
    auto parsed = parse_command(session.fields);
    if (!parsed.ok()) {
      resp::append_error(session.output, parsed.error);
    } else if (parsed.command->type == CommandType::unwatch) {
      // UNWATCH is queueable. EXEC has already removed the transaction's watch
      // registrations, so its ordered result is simply OK.
      resp::append_simple_string(session.output, "OK");
    } else if (is_pubsub_command(parsed.command->type)) {
      pubsub.execute(session, *parsed.command, session.output);
    } else {
      execute_command_into(store, *parsed.command, session.output,
                           queued_options);
    }
    scrub_auth_request(session.fields);
  }
  transaction.finish();
}

[[nodiscard]] bool execute_transaction_control(
    EndpointSession& session, Store& store, detail::PubSubRegistry& pubsub,
    detail::WatchRegistry& watches, const Command& command,
    std::span<const std::string_view> fields,
    const CommandExecutionOptions& exec_options) {
  auto& transaction = session.transaction;
  switch (command.type) {
    case CommandType::multi:
      if (transaction.in_multi) {
        resp::append_error(session.output, "ERR MULTI calls can not be nested");
      } else {
        transaction.begin();
        resp::append_simple_string(session.output, "OK");
      }
      return true;
    case CommandType::exec:
      execute_queued_transaction(session, store, pubsub, watches, exec_options);
      return true;
    case CommandType::discard:
      if (!transaction.in_multi) {
        resp::append_error(session.output, "ERR DISCARD without MULTI");
      } else {
        watches.remove(transaction);
        transaction.finish();
        resp::append_simple_string(session.output, "OK");
      }
      return true;
    case CommandType::watch:
      if (transaction.in_multi) {
        resp::append_error(session.output, "ERR WATCH inside MULTI is not allowed");
        return true;
      }
      for (const auto key : command.args) {
        (void)store.purge_if_expired(key, store.now_ms());
      }
      try {
        watches.watch(transaction, command.args);
        resp::append_simple_string(session.output, "OK");
      } catch (const std::bad_alloc&) {
        resp::append_error(session.output, "ERR unable to register watched keys");
      }
      return true;
    case CommandType::unwatch:
      if (!transaction.in_multi) {
        watches.remove(transaction);
        resp::append_simple_string(session.output, "OK");
        return true;
      }
      break;  // UNWATCH is queued inside MULTI.
    default:
      break;
  }

  if (!transaction.in_multi) {
    return false;
  }
  if (transaction.failure == detail::TransactionFailure::buffer_limit) {
    append_transaction_limit_error(session.output,
                                   transaction.commands.mapped_bytes());
    return true;
  }
  if (!transaction.commands.append(fields)) {
    transaction.failure = detail::TransactionFailure::buffer_limit;
    append_transaction_limit_error(session.output,
                                   transaction.commands.mapped_bytes());
  } else {
    resp::append_simple_string(session.output, "QUEUED");
  }
  return true;
}

void dispatch_resp_command(EndpointSession& session, Store& store,
                           detail::PubSubRegistry& pubsub,
                           detail::WatchRegistry& watches,
                           BlockingListRegistry& blocking_lists,
                           ReplicationRuntime& replication,
                           std::span<const std::string_view> fields,
                           const CommandExecutionOptions& exec_options) {
  const std::size_t prior_size = session.output.size();
  auto parsed = parse_command(fields);
  if (!parsed.ok()) {
    if (session.transaction.in_multi &&
        session.transaction.failure != detail::TransactionFailure::buffer_limit) {
      session.transaction.failure = detail::TransactionFailure::command_error;
    }
    resp::append_error(session.output, parsed.error);
    session.record_reply(prior_size);
    scrub_auth_request(fields);
    return;
  }

  const auto& command = *parsed.command;
  ReplicationRuntime::Scope replication_scope(replication, store);
  if (session.transaction.in_multi && command.type == CommandType::unknown) {
    if (session.transaction.failure != detail::TransactionFailure::buffer_limit) {
      session.transaction.failure = detail::TransactionFailure::command_error;
    }
    execute_command_into(store, command, session.output, exec_options);
    scrub_auth_request(fields);
    session.record_reply(prior_size);
    return;
  }
  if (session.authentication_required && !session.authenticated &&
      !allowed_before_authentication(command.type)) {
    resp::append_error(session.output, "NOAUTH Authentication required.");
  } else if (session.wire_mode == detail::WireMode::resp2 &&
      session.subscription_count() != 0 &&
      !allowed_while_resp2_subscribed(command.type)) {
    resp::append_error(
        session.output,
        "ERR Can't execute this command while subscribed to a channel");
  } else if (session.wire_mode == detail::WireMode::resp2 &&
             session.subscription_count() != 0 &&
             command.type == CommandType::ping) {
    resp::append_array_header(session.output, 2);
    resp::append_bulk_string(session.output, "pong");
    resp::append_bulk_string(session.output,
                             command.args.empty() ? std::string_view{}
                                                  : command.args.front());
  } else if (execute_transaction_control(session, store, pubsub, watches,
                                         command, fields, exec_options)) {
    if (command.type == CommandType::exec) {
      // EXEC reuses session.fields to decode mmap records, invalidating the
      // caller's span. EXEC has no credentials of its own, so skip the common
      // input scrub and finish the reply here.
      if (session.wire_mode != detail::WireMode::sbe) {
        session.wire_mode = session.resp_version == resp::Version::resp3
                                ? detail::WireMode::resp3
                                : detail::WireMode::resp2;
      }
      session.record_reply(prior_size);
      blocking_lists.serve_ready(store, replication);
      return;
    }
  } else if (command.type == CommandType::goblin_firehose) {
    pubsub.remove(session);
    watches.remove(session.transaction);
    blocking_lists.remove(session);
    std::string error;
    if (!replication.subscribe(session, store, error)) {
      resp::append_error(session.output, error);
    }
  } else if (is_pubsub_command(command.type)) {
    pubsub.execute(session, command, session.output);
  } else {
    execute_command_into(store, command, session.output, exec_options);
  }
  scrub_auth_request(fields);

  if (session.wire_mode != detail::WireMode::sbe) {
    session.wire_mode = session.resp_version == resp::Version::resp3
                            ? detail::WireMode::resp3
                            : detail::WireMode::resp2;
  }
  session.record_reply(prior_size);
  blocking_lists.serve_ready(store, replication);
}

#ifdef GOBLIN_HAS_SBE
[[nodiscard]] std::size_t dispatch_sbe_command(
    EndpointSession& session, Store& store, detail::PubSubRegistry& pubsub,
    ReplicationRuntime& replication, std::string_view bytes,
    const CommandExecutionOptions& exec_options) {
  if (bytes.size() < kSbeLenPrefix) {
    return 0;
  }
  std::uint32_t message_length = 0;
  std::memcpy(&message_length, bytes.data(), kSbeLenPrefix);
  const std::size_t frame_bytes = kSbeLenPrefix + message_length;
  if (bytes.size() < frame_bytes) {
    return 0;
  }
  if (message_length < goblin_sbe::MessageHeader::encodedLength()) {
    return frame_bytes;
  }

  char* buffer = const_cast<char*>(bytes.data()) + kSbeLenPrefix;
  ReplicationRuntime::Scope replication_scope(replication, store);
  try {
    goblin_sbe::MessageHeader header(buffer, message_length);
    const auto template_id = header.templateId();
    Command command;

    if (template_id == goblin_sbe::Subscribe::sbeTemplateId()) {
      goblin_sbe::Subscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.channels();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getChannelAsStringView());
      }
      command.type = CommandType::subscribe;
      command.name = "SUBSCRIBE";
    } else if (template_id == goblin_sbe::Unsubscribe::sbeTemplateId()) {
      goblin_sbe::Unsubscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.channels();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getChannelAsStringView());
      }
      command.type = CommandType::unsubscribe;
      command.name = "UNSUBSCRIBE";
    } else if (template_id == goblin_sbe::PSubscribe::sbeTemplateId()) {
      goblin_sbe::PSubscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.patterns();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getPatternAsStringView());
      }
      command.type = CommandType::psubscribe;
      command.name = "PSUBSCRIBE";
    } else if (template_id == goblin_sbe::PUnsubscribe::sbeTemplateId()) {
      goblin_sbe::PUnsubscribe message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      auto& group = message.patterns();
      session.fields.clear();
      session.fields.reserve(static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getPatternAsStringView());
      }
      command.type = CommandType::punsubscribe;
      command.name = "PUNSUBSCRIBE";
    } else if (template_id == goblin_sbe::Publish::sbeTemplateId()) {
      goblin_sbe::Publish message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      session.fields.clear();
      session.fields.push_back(message.getChannelAsStringView());
      session.fields.push_back(message.getPayloadAsStringView());
      command.type = CommandType::publish;
      command.name = "PUBLISH";
    } else if (template_id == goblin_sbe::PubSub::sbeTemplateId()) {
      goblin_sbe::PubSub message;
      message.wrapForDecode(buffer, goblin_sbe::MessageHeader::encodedLength(),
                            header.blockLength(), header.version(), message_length);
      const auto operation = message.operation();
      session.fields.clear();
      if (operation == 0) {
        session.fields.push_back("CHANNELS");
      } else if (operation == 1) {
        session.fields.push_back("NUMSUB");
      } else if (operation == 2) {
        session.fields.push_back("NUMPAT");
      } else {
        session.fields.push_back("UNKNOWN");
      }
      auto& group = message.args();
      session.fields.reserve(1 + static_cast<std::size_t>(group.count()));
      while (group.hasNext()) {
        session.fields.push_back(group.next().getArgAsStringView());
      }
      command.type = CommandType::pubsub;
      command.name = "PUBSUB";
    } else {
      return sbe_dispatch_one(store, bytes, session.output, exec_options);
    }

    command.args = session.fields;
    pubsub.execute(session, command, session.output);
  } catch (const std::exception&) {
    // Consume malformed frames and re-synchronize at the next length prefix.
  }
  return frame_bytes;
}
#endif

[[nodiscard]] bool process_buffered_commands(Client& client,
                                             Store& store,
                                             detail::PubSubRegistry& pubsub,
                                             detail::WatchRegistry& watches,
                                             BlockingListRegistry& blocking_lists,
                                             ScriptEngine& script_engine,
                                             LuauEngine& luau_engine,
                                             WrenEngine& wren_engine,
                                             TclEngine& tcl_engine,
                                             UPythonEngine& upython_engine,
                                             QuickJsEngine& quickjs_engine,
                                             const ServerConfig& config,
                                             const AuthDatabase* auth_database,
                                             ReplicationRuntime& replication,
                                             const NestedCommandDispatch&
                                                 nested_dispatch) {
  if (client.blocked_list || client.firehose) {
    return true;
  }
  compact_output_if_needed(client);
  update_read_backpressure(client, config);

  BlockingListParkContext park_context{.registry = &blocking_lists,
                                       .session = &client};
  const CommandExecutionOptions exec_options{
      .output_reserve_limit = config.max_output_buffer_bytes,
      .resp_version = &client.resp_version,
      .connection_id = client.connection_id,
      .auth_database = auth_database,
      .authenticated = &client.authenticated,
      .authenticated_username = &client.authenticated_username,
      .client_name = &client.client_name,
      .client_library_name = &client.client_library_name,
      .client_library_version = &client.client_library_version,
      .quit_requested = &client.close_after_write,
      .replication_context = nested_dispatch.replication_context,
      .replicate_write = nested_dispatch.replicate_write,
      .read_only = nested_dispatch.read_only,
      .blocking_lists = BlockingListDispatch{.context = &park_context,
                                             .park = park_blocking_list},
      .script_engine = &script_engine,
      .luau_engine = &luau_engine,
      .wren_engine = &wren_engine,
      .tcl_engine = &tcl_engine,
      .upython_engine = &upython_engine,
      .quickjs_engine = &quickjs_engine};

  // Decide the protocol from the first 8 bytes ("GOBLINS!" -> SBE, else RESP),
  // mirroring the ring. Decide RESP the moment the prefix diverges so a short inline
  // command is never stalled waiting for a full magic.
  if (client.wire_mode == detail::WireMode::undecided) {
#ifdef GOBLIN_HAS_SBE
    if (config.enable_sbe) {
      switch (match_goblin_magic(client.inbuf)) {
        case MagicMatch::need_more:
          return true;  // wait for the next read before deciding
        case MagicMatch::yes:
          client.wire_mode = detail::WireMode::sbe;
          client.authentication_required = false;
          client.authenticated = true;
          client.authenticated_username.clear();
          client.inbuf.erase(0, sizeof(kGoblinMagicBytes));  // consume the magic once
          break;
        case MagicMatch::no:
          client.wire_mode = detail::WireMode::resp2;
          break;
      }
    } else {
      client.wire_mode = detail::WireMode::resp2;
    }
#else
    client.wire_mode = detail::WireMode::resp2;
#endif
  }

#ifdef GOBLIN_HAS_SBE
  if (client.wire_mode == detail::WireMode::sbe) {
    // Frame and dispatch every complete SBE message; a partial trailing message stays
    // buffered for the next read.
    std::size_t off = 0;
    while (!client.read_backpressured) {
      const std::size_t prior_size = client.output.size();
      const std::size_t consumed = dispatch_sbe_command(
          client, store, pubsub, replication,
          std::string_view(client.inbuf).substr(off), exec_options);
      if (consumed == 0) {
        break;
      }
      client.record_reply(prior_size);
      blocking_lists.serve_ready(store, replication);
      off += consumed;
      compact_output_if_needed(client);
      update_read_backpressure(client, config);
    }
    if (off > 0) {
      client.inbuf.erase(0, off);
    }
    return true;
  }
#endif

  // RESP: move the accumulated bytes into the parser, then pop and dispatch.
  client.parser.append(client.inbuf);
  secure_zero_memory(client.inbuf.data(), client.inbuf.size());
  client.inbuf.clear();
  while (!client.read_backpressured && !client.blocked_list) {
    if (!client.parser.pop_into(client.fields)) {
      break;
    }
    dispatch_resp_command(client, store, pubsub, watches, blocking_lists,
                          replication, client.fields, exec_options);
    if (client.close_after_write || client.firehose) {
      break;
    }
    compact_output_if_needed(client);
    update_read_backpressure(client, config);
  }

  if (!client.blocked_list && client.parser.has_error()) {
    if (!client.close_after_write) {
      const std::size_t prior_size = client.output.size();
      resp::append_error(client.output, client.parser.error());
      client.record_reply(prior_size);
    }
    client.close_after_write = true;
    update_read_backpressure(client, config);
  }

  if (client.firehose) {
    client.parser.clear();
    client.inbuf.clear();
  }

  return true;
}

// Whether a client has a complete command buffered that can be dispatched without a
// new socket read -- RESP frames queued in the parser, or a full SBE frame in inbuf.
// Drives the post-backpressure drain loop; a partial SBE frame returns false so that
// loop cannot spin.
[[nodiscard]] bool has_buffered_work(const Client& client) {
  if (client.blocked_list || client.firehose) {
    return false;
  }
#ifdef GOBLIN_HAS_SBE
  if (client.wire_mode == detail::WireMode::sbe) {
    if (client.inbuf.size() < kSbeLenPrefix) {
      return false;
    }
    std::uint32_t len = 0;
    std::memcpy(&len, client.inbuf.data(), kSbeLenPrefix);
    return client.inbuf.size() >= kSbeLenPrefix + len;
  }
#endif
  return client.parser.has_queued_frames();
}

[[nodiscard]] bool read_client(Client& client, Store& store,
                               detail::PubSubRegistry& pubsub,
                               detail::WatchRegistry& watches,
                               BlockingListRegistry& blocking_lists,
                               ScriptEngine& script_engine,
                               LuauEngine& luau_engine,
                               WrenEngine& wren_engine,
                               TclEngine& tcl_engine,
                               UPythonEngine& upython_engine,
                               QuickJsEngine& quickjs_engine,
                               const ServerConfig& config,
                               const AuthDatabase* auth_database,
                               ReplicationRuntime& replication,
                               const NestedCommandDispatch& nested_dispatch) {
  const std::size_t bufsize = config.client_read_buffer_bytes != 0 ? config.client_read_buffer_bytes : 16 * 1024;
  static thread_local std::vector<char> buffer;
  if (buffer.size() < bufsize) buffer.resize(bufsize);

  for (;;) {
    if (client.read_backpressured) {
      return true;
    }

    const auto received = transport_read(client, buffer.data(), bufsize);
    if (received.status == TransportIoStatus::Progress && received.bytes > 0) {
      client.inbuf.append(buffer.data(), received.bytes);

      if (!client.blocked_list) {
        if (!process_buffered_commands(client, store, pubsub, watches,
                                       blocking_lists, script_engine,
                                       luau_engine, wren_engine, tcl_engine,
                                       upython_engine, quickjs_engine, config,
                                       auth_database, replication,
                                       nested_dispatch)) {
          return false;
        }
      }
      if (client.close_after_write || client.firehose) {
        return true;
      }
      continue;
    }
    switch (received.status) {
      case TransportIoStatus::WouldBlock:
        return true;
      case TransportIoStatus::Interrupted:
        continue;
      case TransportIoStatus::Progress:
      case TransportIoStatus::Closed:
      case TransportIoStatus::Error:
        return false;
    }
  }
}

[[nodiscard]] bool write_client(Client& client, const ServerConfig& config) {
  if (client.close_requested) {
    return false;
  }
  while (has_pending_output(client)) {
    detail::UnsolicitedOutputQueue::Front push{};
    const bool has_push = peek_unsolicited(client, push);
    const bool regular_pending = has_pending_regular_output(client);
    bool write_push = has_push &&
                      (!regular_pending ||
                       push.sequence <
                           client.replies[client.reply_index].sequence);
#ifdef GOBLIN_HAS_TLS
    if (client.tls != nullptr && client.tls_write_retry_bytes != 0) {
      write_push = client.tls_write_retry_unsolicited;
      if ((write_push && !has_push) || (!write_push && !regular_pending)) {
        return false;
      }
    }
#endif

    std::string_view bytes;
    if (write_push) {
      bytes = push.bytes.substr(client.unsolicited_front_offset);
    } else {
      // A pipeline can leave hundreds of adjacent replies in output. Preserve
      // their sequence relative to Pub/Sub pushes, but send every regular reply
      // before the next push as one byte range instead of one syscall per reply.
      auto last_reply = client.reply_index;
      while (last_reply + 1 < client.replies.size() &&
             (!has_push ||
              client.replies[last_reply + 1].sequence < push.sequence)) {
        ++last_reply;
      }
      const auto end = client.replies[last_reply].end_offset;
      bytes = std::string_view(client.output).substr(client.output_offset,
                                                     end - client.output_offset);
    }

#ifdef GOBLIN_HAS_TLS
    if (client.tls != nullptr && client.tls_write_retry_bytes != 0) {
      if (bytes.size() < client.tls_write_retry_bytes) {
        return false;
      }
      bytes = bytes.substr(0, client.tls_write_retry_bytes);
    }
#endif
    const auto sent =
        transport_write(client, bytes.data(), bytes.size(), write_push);
    if (sent.status == TransportIoStatus::Progress && sent.bytes > 0) {
      const auto written = sent.bytes;
      if (write_push) {
        client.unsolicited_front_offset += written;
        if (client.unsolicited_front_offset == push.bytes.size()) {
          consume_unsolicited_front(client);
        }
      } else {
        client.output_offset += written;
        while (client.reply_index < client.replies.size() &&
               client.output_offset >=
                   client.replies[client.reply_index].end_offset) {
          ++client.reply_index;
        }
      }
      update_read_backpressure(client, config);
      continue;
    }
    switch (sent.status) {
      case TransportIoStatus::WouldBlock:
        compact_output_if_needed(client);
        update_read_backpressure(client, config);
        return true;
      case TransportIoStatus::Interrupted:
        continue;
      case TransportIoStatus::Progress:
      case TransportIoStatus::Closed:
      case TransportIoStatus::Error:
        return false;
    }
  }

  compact_output_if_needed(client);
  update_read_backpressure(client, config);
  return !client.close_after_write;
}

#ifdef GOBLIN_HAS_EXASOCK
// Create a non-blocking TCP listener for a priority ExaSock poll target.
[[nodiscard]] std::optional<int> create_tcp_listener(std::string_view address,
                                                     std::uint16_t port,
                                                     int backlog) {
  const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    std::cerr << "goblin-core: socket failed: " << std::strerror(errno) << '\n';
    return std::nullopt;
  }
  int reuse = 1;
  if (::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) != 0) {
    std::cerr << "goblin-core: setsockopt(SO_REUSEADDR) failed: "
              << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  set_no_sigpipe(fd);
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  const std::string address_storage(address);
  if (::inet_pton(AF_INET, address_storage.c_str(), &addr.sin_addr) != 1) {
    std::cerr << "goblin-core: invalid IPv4 bind address: " << address << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (::bind(fd, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0) {
    std::cerr << "goblin-core: bind " << address << ':' << port
              << " failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (::listen(fd, backlog) != 0) {
    std::cerr << "goblin-core: listen failed: " << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  if (!set_nonblocking(fd)) {
    std::cerr << "goblin-core: failed to set listener nonblocking: "
              << std::strerror(errno) << '\n';
    close_fd(fd);
    return std::nullopt;
  }
  return fd;
}
#endif

// Polled mode. Create shared-memory rings, ExaSock TCP listeners, and RDMA
// listeners in their literal command-line order, then process one fragment from
// the highest-priority non-empty target and restart the scan. Only when every
// target is empty do we run one non-blocking plain-socket pass and cpu_relax().
template <class ReplicaFn, class NetFn>
[[nodiscard]] bool run_polled_targets(
    const ServerConfig& config, Store& store, std::atomic_bool& running,
    detail::PubSubRegistry& pubsub, detail::WatchRegistry& watches,
    BlockingListRegistry& blocking_lists,
    ScriptEngine& script_engine,
    LuauEngine& luau_engine, WrenEngine& wren_engine, TclEngine& tcl_engine,
    UPythonEngine& upython_engine, QuickJsEngine& quickjs_engine,
    const AuthDatabase* auth_database,
    ReplicationRuntime& replication,
    const NestedCommandDispatch& nested_dispatch,
    ReplicaFn&& replica_iteration,
    NetFn&& network_iteration) {
  // macOS: same QoS as the client so both busy-pollers stay on P-cores. No-op
  // on Linux (pinning is done by the bench via taskset).
  ring::set_busy_poll_thread_realtime();

  std::vector<PolledRuntimeTarget> targets;
  targets.reserve(config.poll_targets.size());
  // Strictly bind allocations to the selected slice while polled targets are
  // created. main resolved this from --numa, --cpu, or unanimous hardware locality.
  const int ring_node = config.numa_node;
  if (ring_node >= 0) {
    (void)numa::bind_process_to_node(ring_node);
  }
  for (const auto& configured_target : config.poll_targets) {
    if (const auto* rc = std::get_if<RingConfig>(&configured_target)) {
      std::optional<ring::Mapping> mapping;
#if defined(__linux__)
      if (config.ring_hugetlb) {
        // Huge-page backing: create_hugetlb rounds the size up to the huge page,
        // places the file on a hugetlbfs mount, and symlinks rc.path to it.
        mapping = ring::Mapping::create_hugetlb(rc->path.c_str(), rc->bytes);
        if (!mapping) {
          std::cerr << "goblin-core: failed to create hugetlb ring " << rc->path
                    << " (no hugetlbfs mount, or no huge pages reserved -- see "
                       "/proc/meminfo HugePages_Free)\n";
          return false;
        }
      }
#endif
      if (!mapping) {
        const std::uint64_t cap = ring::capacity_for(rc->bytes);
        mapping = ring::Mapping::create(rc->path.c_str(), cap, cap);
      }
      if (!mapping) {
        std::cerr << "goblin-core: failed to create ring " << rc->path << ": "
                  << std::strerror(errno) << '\n';
        return false;
      }
      if (ring_node >= 0 && !mapping->numa_all_local(ring_node)) {
        std::cerr << "goblin-core: ring " << rc->path
                  << " could not be placed on NUMA node " << ring_node
                  << "; refusing to run a remote ring -- reserve memory"
                     " (or huge pages) on that node\n";
        return false;
      }
      std::cout << "goblin-core: ring " << rc->path << " ready ("
                << mapping->sq_capacity() << " bytes/direction"
                << (mapping->is_hugetlb() ? ", hugetlb" : "") << ")\n";
      try {
        PolledRuntimeTarget target;
        target.ring_endpoint = std::make_unique<RingEndpoint>(
            std::move(*mapping), config.unsolicited_output_buffer_bytes,
            config.transaction_buffer_bytes, next_connection_id(),
            auth_database != nullptr && !config.no_auth_ring);
        targets.push_back(std::move(target));
      } catch (const std::bad_alloc&) {
        std::cerr << "goblin-core: unable to map ring client buffers\n";
        return false;
      }
      continue;
    }

    if (const auto* ec = std::get_if<ExasockConfig>(&configured_target)) {
#ifdef GOBLIN_HAS_EXASOCK
      auto listener_fd =
          create_tcp_listener(ec->bind_address, ec->port, config.backlog);
      if (!listener_fd) {
        return false;
      }
      std::cout << "goblin-core: exasock " << ec->bind_address << ':'
                << ec->port << " ready (priority TCP";
      if (exasock::loaded()) {
        std::cout << ", ExaSock " << exasock::version_text() << " loaded";
      } else {
        std::cout << "; run under `exasock` for SmartNIC bypass";
      }
      std::cout << ")\n";
      PolledRuntimeTarget target;
      target.exasock_target = std::make_unique<ExasockRuntimeTarget>();
      target.exasock_target->listener_fd = *listener_fd;
      targets.push_back(std::move(target));
#else
      (void)ec;
      std::cerr << "goblin-core: this build cannot create an ExaSock poll "
                   "target (-DGOBLIN_CORE_ENABLE_EXASOCK=ON required)\n";
      return false;
#endif
      continue;
    }

    const auto& rc = std::get<RdmaConfig>(configured_target);
#ifdef GOBLIN_HAS_RDMA
    std::string error;
    auto listener = rdma::ServerListener::create(
        rc.bind_address, rc.port, rc.bytes, config.backlog, config.numa_node,
        error);
    if (!listener) {
      std::cerr << "goblin-core: failed to create RDMA listener "
                << rc.bind_address << ':' << rc.port << ": " << error << '\n';
      return false;
    }
    std::cout << "goblin-core: RDMA " << rc.bind_address << ':' << rc.port
              << " ready (" << rc.bytes << " slot bytes/peer/direction)\n";
    PolledRuntimeTarget target;
    target.rdma_target = std::make_unique<RdmaRuntimeTarget>();
    target.rdma_target->listener = std::move(listener);
    targets.push_back(std::move(target));
#else
    (void)rc;
    std::cerr << "goblin-core: this build cannot create an RDMA poll target\n";
    return false;
#endif
  }
  if (ring_node >= 0) {
    // Rings placed; restore the runtime allocation policy. Prefer the pinned node for
    // the arenas only when --numa-arena asked for it; otherwise go back to default.
    if (config.numa_arena) {
      (void)numa::prefer_node(ring_node);
    } else {
      numa::reset_policy();
    }
  }

#ifdef GOBLIN_HAS_SBE
  std::unique_ptr<detail::PubSubListenerRuntime> pubsub_listener;
  if (config.pubsub_listener) {
    try {
      pubsub_listener = std::make_unique<detail::PubSubListenerRuntime>(
          *config.pubsub_listener, config.pubsub_listener_pattern);
      std::cout << "goblin-core: Pub/Sub listener connected to "
                << pubsub_listener->description() << " and subscribed to pattern '"
                << config.pubsub_listener_pattern << "' over SBE\n";
    } catch (const std::exception& error) {
      std::cerr << "goblin-core: Pub/Sub listener setup failed: "
                << error.what() << '\n';
      return false;
    }
  }
#endif

  std::cout << "goblin-core: polled mode -- busy-polling "
            << targets.size()
            << " ordered server target(s)"
            << (config.pubsub_listener ? " plus one Pub/Sub listener" : "")
            << " ahead of sockets (100% CPU by design)\n"
            << std::flush;

  // Drain one SQ record from `ep`, run whatever commands it completes, and push
  // the replies onto the CQ. Returns false when the ring's SQ was empty.
  const auto flush_polled_output = [&]<class Producer>(
                                       EndpointSession& ep,
                                       Producer& producer) -> bool {
    bool progressed = false;
    while (has_pending_output(ep)) {
      detail::UnsolicitedOutputQueue::Front push{};
      const bool has_push = peek_unsolicited(ep, push);
      const bool regular_pending = has_pending_regular_output(ep);
      const bool write_push = has_push &&
                              (!regular_pending ||
                               push.sequence < ep.replies[ep.reply_index].sequence);

      std::string_view bytes;
      if (write_push) {
        bytes = push.bytes.substr(ep.unsolicited_front_offset);
      } else {
        const auto end = ep.replies[ep.reply_index].end_offset;
        bytes = std::string_view(ep.output).substr(ep.output_offset,
                                                   end - ep.output_offset);
      }
      if (bytes.size() > producer.max_record_payload()) {
        bytes = bytes.substr(0, producer.max_record_payload());
      }
      if (!producer.try_push(bytes)) {
        break;
      }
      progressed = true;
      if (write_push) {
        ep.unsolicited_front_offset += bytes.size();
        if (ep.unsolicited_front_offset == push.bytes.size()) {
          consume_unsolicited_front(ep);
        }
      } else {
        ep.output_offset += bytes.size();
        if (ep.output_offset == ep.replies[ep.reply_index].end_offset) {
          ++ep.reply_index;
        }
      }
      compact_output_if_needed(ep);
    }
    return progressed;
  };

  const auto process_polled_endpoint = [&]<class Consumer, class Producer>(
                                           EndpointSession& ep,
                                           Consumer& consumer,
                                           Producer& producer) -> bool {
    const bool output_progress = flush_polled_output(ep, producer);
    if (ep.quit_after_write && !has_pending_output(ep)) {
      ep.close_requested = true;
      return true;
    }
    if (ep.close_requested || has_pending_output(ep)) {
      return output_progress;
    }
    if (ep.firehose) {
      return output_progress;
    }
    if (ep.blocked_list) {
      return output_progress;
    }

    BlockingListParkContext park_context{.registry = &blocking_lists,
                                         .session = &ep};
    const CommandExecutionOptions exec_options{
        .output_reserve_limit = config.max_output_buffer_bytes,
        .resp_version = &ep.resp_version,
        .connection_id = ep.connection_id,
        .auth_database = auth_database,
        .authenticated = &ep.authenticated,
        .authenticated_username = &ep.authenticated_username,
        .client_name = &ep.client_name,
        .client_library_name = &ep.client_library_name,
        .client_library_version = &ep.client_library_version,
        .quit_requested = &ep.quit_after_write,
        .replication_context = nested_dispatch.replication_context,
        .replicate_write = nested_dispatch.replicate_write,
        .read_only = nested_dispatch.read_only,
        .blocking_lists = BlockingListDispatch{.context = &park_context,
                                               .park = park_blocking_list},
        .script_engine = &script_engine,
        .luau_engine = &luau_engine,
        .wren_engine = &wren_engine,
        .tcl_engine = &tcl_engine,
        .upython_engine = &upython_engine,
        .quickjs_engine = &quickjs_engine};

    const auto record = consumer.peek();
    if (!record) {
      return false;
    }

    // Decide the protocol from the first 8 bytes: "GOBLINS!" -> the SBE binary wire,
    // else RESP. Decide RESP the moment the prefix diverges so a short inline RESP
    // command is never stalled waiting for a full magic that will not arrive.
    if (ep.wire_mode == detail::WireMode::undecided) {
      ep.inbuf.append(*record);
      consumer.pop();
#ifdef GOBLIN_HAS_SBE
      if (config.enable_sbe) {
        switch (match_goblin_magic(ep.inbuf)) {
          case MagicMatch::need_more:
            return true;  // matches so far but incomplete -> wait for the next record
          case MagicMatch::yes:
            ep.wire_mode = detail::WireMode::sbe;
            ep.authentication_required = false;
            ep.authenticated = true;
            ep.authenticated_username.clear();
            ep.inbuf.erase(0, sizeof(kGoblinMagicBytes));  // consume the magic once
            break;
          case MagicMatch::no:
            ep.wire_mode = detail::WireMode::resp2;
            break;
        }
      } else {
        ep.wire_mode = detail::WireMode::resp2;
      }
#else
      ep.wire_mode = detail::WireMode::resp2;
#endif
      // Magic may have been alone in the record; fall through to drain whatever
      // remains in inbuf (or nothing).
    } else {
#ifdef GOBLIN_HAS_SBE
      // Hot path (protocol already decided, no carry-over): dispatch SBE frames
      // straight out of the ring record -- no inbuf memcpy. Partial trailing
      // bytes fall back into inbuf for the next record.
      if (ep.wire_mode == detail::WireMode::sbe && ep.inbuf.empty()) {
        std::size_t off = 0;
        for (;;) {
          const std::size_t prior_size = ep.output.size();
          const std::size_t consumed = dispatch_sbe_command(
              ep, store, pubsub, replication,
              std::string_view(*record).substr(off), exec_options);
          if (consumed == 0) {
            break;
          }
          ep.record_reply(prior_size);
          blocking_lists.serve_ready(store, replication);
          off += consumed;
        }
        if (off == record->size()) {
          consumer.pop();
        } else if (off > 0) {
          ep.inbuf.assign(record->data() + off, record->size() - off);
          consumer.pop();
        } else {
          // Incomplete first frame: buffer the whole record.
          ep.inbuf.assign(record->data(), record->size());
          consumer.pop();
        }
      } else
#endif
      {
        ep.inbuf.append(*record);
        consumer.pop();
      }
    }

#ifdef GOBLIN_HAS_SBE
    if (ep.wire_mode == detail::WireMode::sbe) {
      // Frame and dispatch every complete SBE message still in the accumulator
      // (carry-over from a prior partial frame, or bytes left after magic strip).
      if (!ep.inbuf.empty()) {
        std::size_t off = 0;
        for (;;) {
          const std::size_t prior_size = ep.output.size();
          const std::size_t consumed = dispatch_sbe_command(
              ep, store, pubsub, replication,
              std::string_view(ep.inbuf).substr(off), exec_options);
          if (consumed == 0) {
            break;
          }
          ep.record_reply(prior_size);
          blocking_lists.serve_ready(store, replication);
          off += consumed;
        }
        if (off > 0) {
          ep.inbuf.erase(0, off);
        }
      }
    } else
#endif
    if (ep.wire_mode == detail::WireMode::resp2 ||
        ep.wire_mode == detail::WireMode::resp3) {
      ep.parser.append(ep.inbuf);
      secure_zero_memory(ep.inbuf.data(), ep.inbuf.size());
      ep.inbuf.clear();
      while (!ep.blocked_list && ep.parser.pop_into(ep.fields)) {
        dispatch_resp_command(ep, store, pubsub, watches, blocking_lists,
                              replication, ep.fields, exec_options);
        if (ep.quit_after_write || ep.blocked_list || ep.firehose) {
          break;
        }
      }
      if (ep.firehose) {
        ep.parser.clear();
        ep.inbuf.clear();
      }
      if (!ep.blocked_list && ep.parser.has_error()) {
        const std::size_t prior_size = ep.output.size();
        resp::append_error(ep.output, ep.parser.error());
        ep.record_reply(prior_size);
        ep.parser.clear();  // resync the byte stream after a protocol error
      }
    }

    (void)flush_polled_output(ep, producer);
    return true;
  };

  const auto process_ring = [&](RingEndpoint& ep) -> bool {
    // A fresh shared-memory client asks the server to discard anything its dead
    // predecessor abandoned before it starts using the SPSC ring.
    if (const std::uint64_t epoch = ep.mapping.requested_epoch();
        epoch != ep.acked_epoch) {
      replication.remove(ep);
      pubsub.remove(ep);
      watches.remove(ep.transaction);
      blocking_lists.remove(ep);
      ep.mapping.drain_for_reconnect();
      ep.reset_connection(next_connection_id());
      ep.mapping.ack_epoch(epoch);
      ep.acked_epoch = epoch;
      ep.rebind_ring_views();
    }
    return process_polled_endpoint(ep, ep.sq, ep.cq);
  };

#ifdef GOBLIN_HAS_RDMA
  const auto process_rdma_target = [&](RdmaRuntimeTarget& target) -> bool {
    bool progressed = false;
    auto event = target.listener->poll();
    progressed = event.progressed;
    if (!target.listener->error().empty() && !target.listener_error_reported) {
      std::cerr << "goblin-core: RDMA listener error: "
                << target.listener->error() << '\n';
      target.listener_error_reported = true;
      running = false;
      return true;
    }
    if (event.connection) {
      try {
        target.endpoints.push_back(std::make_unique<RdmaEndpoint>(
            std::move(event.connection), config.unsolicited_output_buffer_bytes,
            config.transaction_buffer_bytes, next_connection_id(),
            auth_database != nullptr && !config.no_auth_rdma));
      } catch (const std::bad_alloc&) {
        std::cerr << "goblin-core: unable to allocate RDMA endpoint state\n";
        running = false;
      }
      progressed = true;
    }

    for (std::size_t i = target.endpoints.size(); i > 0; --i) {
      const std::size_t index = i - 1;
      auto& endpoint = *target.endpoints[index];
      if (!endpoint.connection->failed() &&
          !endpoint.connection->disconnected()) {
        continue;
      }
      if (endpoint.connection->failed() && !endpoint.connection->error().empty()) {
        std::cerr << "goblin-core: RDMA peer closed after error: "
                  << endpoint.connection->error() << '\n';
      }
      replication.remove(endpoint);
      pubsub.remove(endpoint);
      watches.remove(endpoint.transaction);
      blocking_lists.remove(endpoint);
      target.endpoints.erase(target.endpoints.begin() +
                             static_cast<std::ptrdiff_t>(index));
      if (target.next_endpoint > index) {
        --target.next_endpoint;
      }
      progressed = true;
    }
    if (target.endpoints.empty()) {
      target.next_endpoint = 0;
      return progressed;
    }
    target.next_endpoint %= target.endpoints.size();

    const std::size_t count = target.endpoints.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const std::size_t index = (target.next_endpoint + offset) % count;
      auto& endpoint = *target.endpoints[index];
      if (!endpoint.connection->established()) {
        continue;
      }
      if (endpoint.close_requested && !has_pending_output(endpoint)) {
        if (!endpoint.disconnect_started) {
          endpoint.connection->disconnect();
          endpoint.disconnect_started = true;
          target.next_endpoint = (index + 1) % count;
          return true;
        }
        continue;
      }
      if (process_polled_endpoint(endpoint, *endpoint.connection,
                                  *endpoint.connection)) {
        target.next_endpoint = (index + 1) % count;
        return true;
      }
    }
    return progressed;
  };
#endif

#ifdef GOBLIN_HAS_EXASOCK
  // One unit of work on a priority ExaSock TCP target: drain accepts, then
  // service one client that has readable/writable progress (round-robin).
  const auto process_exasock_target = [&](ExasockRuntimeTarget& target) -> bool {
    bool progressed = false;
    const auto before_accept = target.clients.size();
    accept_clients(target.listener_fd, true, target.clients, config);
    if (target.clients.size() > before_accept) {
      progressed = true;
    }

    // Close finished clients.
    for (std::size_t i = target.clients.size(); i > 0; --i) {
      const std::size_t index = i - 1;
      auto& client = *target.clients[index];
      if (client.close_requested && !has_pending_output(client)) {
        replication.remove(client);
        pubsub.remove(client);
        watches.remove(client.transaction);
        blocking_lists.remove(client);
        close_fd(client.fd);
        target.clients.erase(target.clients.begin() +
                             static_cast<std::ptrdiff_t>(index));
        if (target.next_client > index) {
          --target.next_client;
        }
        progressed = true;
      }
    }
    if (target.clients.empty()) {
      target.next_client = 0;
      return progressed;
    }
    target.next_client %= target.clients.size();

    const std::size_t count = target.clients.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
      const std::size_t index = (target.next_client + offset) % count;
      auto& client = *target.clients[index];
      update_read_backpressure(client, config);

      bool keep = !client.close_requested;
      bool did_work = false;

      if (keep && has_pending_output(client)) {
        const bool before_pending = has_pending_output(client);
        keep = write_client(client, config);
        if (before_pending) {
          did_work = true;
        }
      }

      if (keep && !client.read_backpressured && has_buffered_work(client)) {
        keep = process_buffered_commands(
            client, store, pubsub, watches, blocking_lists, script_engine,
            luau_engine,
            wren_engine, tcl_engine, upython_engine, quickjs_engine, config,
            auth_database, replication, nested_dispatch);
        did_work = true;
      }

      // Non-blocking readiness probe (timeout 0) so we only enter read_client
      // when the socket has data -- matches the ring/RDMA "one fragment" unit.
      pollfd pfd{.fd = client.fd, .events = 0, .revents = 0};
      if (keep && !client.firehose && !client.read_backpressured) {
        pfd.events |= POLLIN;
      }
      if (keep && has_pending_output(client)) {
        pfd.events |= POLLOUT;
      }
      if (pfd.events != 0) {
        (void)::poll(&pfd, 1, 0);
      }

      if (keep && (pfd.revents & POLLIN) != 0 && !client.read_backpressured) {
        keep = read_client(client, store, pubsub, watches, blocking_lists,
                           script_engine, luau_engine, wren_engine, tcl_engine,
                           upython_engine, quickjs_engine, config,
                           auth_database, replication, nested_dispatch);
        did_work = true;
      }
      if (keep && (pfd.revents & POLLOUT) != 0 && has_pending_output(client)) {
        keep = write_client(client, config);
        did_work = true;
      }

      if (!keep || client.close_requested) {
        replication.remove(client);
        pubsub.remove(client);
        watches.remove(client.transaction);
        blocking_lists.remove(client);
        close_fd(client.fd);
        target.clients.erase(target.clients.begin() +
                             static_cast<std::ptrdiff_t>(index));
        if (target.next_client > index) {
          --target.next_client;
        } else if (!target.clients.empty()) {
          target.next_client %= target.clients.size();
        } else {
          target.next_client = 0;
        }
        return true;
      }

      if (did_work) {
        target.next_client =
            target.clients.empty() ? 0 : (index + 1) % target.clients.size();
        return true;
      }
    }
    return progressed;
  };
#endif

  // macOS has no core pinning; raise this busy-poll thread's priority so the scheduler
  // stops parking it mid-spin (a no-op elsewhere -- Linux uses taskset/isolcpus).
  ring::set_busy_poll_thread_realtime();

  // Idle spins between polled requests: running network_iteration (poll + active
  // expire) every empty loop is a p99 footgun. Service sockets every 64th idle
  // pass; a busy higher-priority target still starves everything below by design.
  //
  // Pure spin (no park): a multi-ring server cannot block on one SQ without
  // missing the others. On Apple Silicon cpu_relax() is a compiler barrier (not
  // YIELD), so the spin does not volunteer deschedule; the client's adaptive
  // wait parks the other side of the handoff when the peer is stalled.
  unsigned idle_spins = 0;
  while (running.load(std::memory_order_relaxed)) {
    if (replica_iteration()) {
      idle_spins = 0;
      continue;
    }
    bool progressed = false;
    for (auto& target : targets) {
      bool target_progress = false;
      if (target.ring_endpoint) {
        target_progress = process_ring(*target.ring_endpoint);
      }
#ifdef GOBLIN_HAS_EXASOCK
      else if (target.exasock_target) {
        target_progress = process_exasock_target(*target.exasock_target);
      }
#endif
#ifdef GOBLIN_HAS_RDMA
      else if (target.rdma_target) {
        target_progress = process_rdma_target(*target.rdma_target);
      }
#endif
      if (target_progress) {
        progressed = true;
        break;  // restart from the highest-priority configured target
      }
    }

#ifdef GOBLIN_HAS_SBE
    bool listener_progress = false;
    if (pubsub_listener) {
      try {
        listener_progress = pubsub_listener->rebroadcast_one(pubsub);
      } catch (const std::exception& error) {
        std::cerr << "goblin-core: Pub/Sub listener failed: " << error.what()
                  << '\n';
        running = false;
        break;
      }
      if (listener_progress) {
        // Relaying can enqueue output for ordinary TCP/UDS subscribers. Service
        // them now even when the upstream stream never goes idle.
        network_iteration(0);
        progressed = true;
      }
    }
#endif

    if (progressed) {
      idle_spins = 0;
      continue;
    }
    if ((++idle_spins & 63u) == 0) {
      network_iteration(0);  // sparse network pass while rings are quiet
    }
    ring::cpu_relax();
  }
  for (auto& target : targets) {
    if (target.ring_endpoint) {
      replication.remove(*target.ring_endpoint);
      pubsub.remove(*target.ring_endpoint);
      watches.remove(target.ring_endpoint->transaction);
      blocking_lists.remove(*target.ring_endpoint);
    }
#ifdef GOBLIN_HAS_EXASOCK
    if (target.exasock_target) {
      for (auto& client : target.exasock_target->clients) {
        replication.remove(*client);
        pubsub.remove(*client);
        watches.remove(client->transaction);
        blocking_lists.remove(*client);
        close_fd(client->fd);
        client->fd = -1;
      }
      close_fd(target.exasock_target->listener_fd);
      target.exasock_target->listener_fd = -1;
    }
#endif
#ifdef GOBLIN_HAS_RDMA
    if (target.rdma_target) {
      for (auto& endpoint : target.rdma_target->endpoints) {
        replication.remove(*endpoint);
        pubsub.remove(*endpoint);
        watches.remove(endpoint->transaction);
        blocking_lists.remove(*endpoint);
      }
    }
#endif
  }
  return true;
}

}  // namespace

Server::Server(ServerConfig config, Store& store)
    : config_(std::move(config)), store_(store) {
  normalize_socket_listeners(config_);
  const std::size_t page = static_cast<std::size_t>(ring::page_size());
  const auto page_round = [page](std::size_t requested) {
    if (requested == 0) {
      return page;
    }
    if (requested <= std::numeric_limits<std::size_t>::max() - (page - 1)) {
      return ((requested + page - 1) / page) * page;
    }
    return (std::numeric_limits<std::size_t>::max() / page) * page;
  };
  config_.unsolicited_output_buffer_bytes =
      page_round(config_.unsolicited_output_buffer_bytes);
  config_.replication_buffer_bytes = page_round(
      config_.replication_buffer_bytes == 0 ? 1024U * 1024U
                                            : config_.replication_buffer_bytes);
  config_.transaction_buffer_bytes =
      page_round(config_.transaction_buffer_bytes);
  if (config_.max_output_buffer_bytes == 0) {
    config_.resume_output_buffer_bytes = 0;
  } else if (config_.resume_output_buffer_bytes >= config_.max_output_buffer_bytes) {
    config_.resume_output_buffer_bytes = config_.max_output_buffer_bytes / 4;
  }
  if (config_.max_output_buffer_bytes > 0 &&
      config_.initial_output_buffer_bytes > config_.max_output_buffer_bytes) {
    config_.initial_output_buffer_bytes = config_.max_output_buffer_bytes;
  }
}

void Server::stop() noexcept {
  running_ = false;
}

int Server::run() {
  std::signal(SIGPIPE, SIG_IGN);
  bool kafka_failed = false;

#ifndef GOBLIN_HAS_SBE
  if (config_.enable_sbe) {
    std::cerr << "goblin-core: --enable-sbe requires an SBE-enabled build\n";
    return 1;
  }
#endif
  if (config_.pubsub_listener && !config_.enable_sbe) {
    std::cerr << "goblin-core: Pub/Sub listener transports require --enable-sbe\n";
    return 1;
  }

  bool has_tls_listener = false;
  for (const auto& configured : config_.socket_listeners) {
    const auto* tcp = std::get_if<TcpListenerConfig>(&configured);
    if (tcp == nullptr) {
      continue;
    }
    if (tcp->bind_address == "0.0.0.0") {
      std::cerr << "goblin-core: 0.0.0.0 cannot preserve the mandatory "
                   "plaintext 127.0.0.1 listener; specify concrete interface "
                   "addresses instead\n";
      return 1;
    }
    has_tls_listener = has_tls_listener || tcp->tls;
  }
  if (has_tls_listener && !config_.tls) {
    std::cerr << "goblin-core: non-loopback TCP listeners require a TLS "
                 "certificate and private key\n";
    return 1;
  }

#ifdef GOBLIN_HAS_TLS
  TlsContextPtr tls_context(nullptr, &SSL_CTX_free);
  if (config_.tls) {
    std::string error;
    tls_context = create_tls_context(*config_.tls, error);
    if (!tls_context) {
      std::cerr << "goblin-core: TLS setup failed: " << error << '\n';
      return 1;
    }
    std::cout << "goblin-core: TLS 1.2+ enabled for non-loopback TCP "
                 "listeners\n";
  }
#else
  if (config_.tls || has_tls_listener) {
    std::cerr << "goblin-core: TLS requires a build configured with "
                 "-DGOBLIN_CORE_ENABLE_TLS=ON\n";
    return 1;
  }
#endif

  std::optional<AuthDatabase> auth_database;
  if (config_.auth_file) {
    try {
      auth_database.emplace(AuthDatabase::load(*config_.auth_file));
    } catch (const std::exception& error) {
      std::cerr << "goblin-core: cannot load auth file: " << error.what() << '\n';
      return 1;
    }
    std::cout << "goblin-core: RESP authentication enabled ("
              << auth_database->size() << " user(s))\n";
  }
  if (config_.enable_sbe) {
    std::cout << "goblin-core: SBE enabled as a trusted, unauthenticated fabric\n";
  }
  const AuthDatabase* auth = auth_database ? &*auth_database : nullptr;

  std::unique_ptr<ReplicationFollowerRuntime> replica;
  if (config_.replica_source) {
    try {
      replica = std::make_unique<ReplicationFollowerRuntime>(
          *config_.replica_source, config_.replication_buffer_bytes,
          config_.replica_auth ? &*config_.replica_auth : nullptr);
      if (config_.replica_auth) {
        std::fill(config_.replica_auth->password.begin(),
                  config_.replica_auth->password.end(), '\0');
        config_.replica_auth->password.clear();
      }
    } catch (const std::exception& error) {
      std::cerr << "goblin-core: replica setup failed: " << error.what() << '\n';
      return 1;
    }
    const auto& hello = replica->hello();
    const auto state = store_.replication_state();
    if (state.valid && state.id != hello.id) {
      std::cerr << "goblin-core: replica snapshot lineage " << state.id.hex()
                << " does not match upstream " << hello.id.hex() << '\n';
      return 1;
    }
    if (!state.valid) {
      store_.set_replication_state(ReplicationState{
          .id = hello.id,
          .offset = 0,
          .kafka_acknowledged_offset = -1,
          .valid = true});
    }
    std::cout << "goblin-core: connected replica source "
              << replica->description() << " at replication offset "
              << hello.offset << " (lineage " << hello.id.hex() << ")\n";
  }

#ifdef GOBLIN_HAS_KAFKA
  std::unique_ptr<KafkaIngestor> kafka;
  std::unique_ptr<KafkaJournal> kafka_journal;
  if (config_.kafka) {
    std::string error;
    if (config_.kafka->acknowledged_offset) {
      std::cout << "goblin-core: resuming Kafka inclusively at broker offset "
                << *config_.kafka->acknowledged_offset << " for replication "
                << store_.replication_state().id.hex() << " after logical offset "
                << store_.replication_state().offset << '\n';
    }
    kafka = KafkaIngestor::connect(config_.kafka->connection,
                                   config_.kafka->start_timestamp_ms,
                                   config_.kafka->acknowledged_offset,
                                   config_.kafka->require_replication_metadata,
                                   error);
    if (!kafka) {
      std::cerr << "goblin-core: Kafka setup failed: " << error << '\n';
      return 1;
    }
    KafkaReplayStats replay;
    std::cout << "goblin-core: replaying Kafka " << kafka->description()
              << " before opening listeners\n" << std::flush;
    const auto buffer_replica = [](void* context, std::string& observer_error) {
      return static_cast<ReplicationFollowerRuntime*>(context)
          ->buffer_available(observer_error);
    };
    if (!kafka->catch_up(store_, replay, error, replica.get(),
                         replica ? buffer_replica : nullptr)) {
      std::cerr << "goblin-core: Kafka startup replay failed: " << error << '\n';
      return 1;
    }
    std::cout << "goblin-core: Kafka startup replay complete: "
              << replay.records << " record(s), " << replay.writes
              << " write(s), " << replay.filtered << " filtered\n";
    if (!store_.replication_state().valid) {
      store_.reset_replication_identity();
    }
    if (!replica) {
      kafka_journal =
          KafkaJournal::connect(config_.kafka->connection, error);
      if (!kafka_journal) {
        std::cerr << "goblin-core: Kafka journal setup failed: " << error << '\n';
        return 1;
      }
    }
  }
#else
  if (config_.kafka) {
    std::cerr << "goblin-core: this build has no Kafka support\n";
    return 1;
  }
#endif

  if (replica) {
    std::string error;
    bool kafka_handoff = false;
#ifdef GOBLIN_HAS_KAFKA
    if (kafka) {
      kafka_handoff = true;
      const auto deadline = std::chrono::steady_clock::now() +
                            std::chrono::seconds(30);
      while (store_.replication_state().offset < replica->hello().offset) {
        if (!replica->buffer_available(error)) {
          std::cerr << "goblin-core: replica catch-up buffering failed: "
                    << error << '\n';
          return 1;
        }
        auto result = kafka->poll(store_, 4096);
        if (!result.ok()) {
          std::cerr << "goblin-core: replica Kafka handoff failed: "
                    << result.error << '\n';
          return 1;
        }
        if (result.stats.records == 0) {
          if (std::chrono::steady_clock::now() >= deadline) {
            std::cerr << "goblin-core: Kafka did not reach upstream replication "
                         "offset "
                      << replica->hello().offset << " within 30 seconds\n";
            return 1;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
      }
    }
#endif
    const bool handoff_offset_invalid = kafka_handoff
                                            ? store_.replication_state().offset <
                                                  replica->hello().offset
                                            : store_.replication_state().offset !=
                                                  replica->hello().offset;
    if (handoff_offset_invalid) {
      std::cerr << "goblin-core: upstream is at replication offset "
                << replica->hello().offset << ", but the loaded snapshot is at "
                << store_.replication_state().offset
                << "; configure Kafka recovery or load a current snapshot\n";
      return 1;
    }

    if (!replica->buffer_available(error)) {
      std::cerr << "goblin-core: replica handoff buffering failed: " << error
                << '\n';
      return 1;
    }
    for (;;) {
      ReplicationBatch batch;
      bool available = false;
      if (!replica->try_next(batch, available, error)) {
        std::cerr << "goblin-core: replica handoff failed: " << error << '\n';
        return 1;
      }
      if (!available) break;
      if (!apply_firehose_batch(store_, batch, error)) {
        std::cerr << "goblin-core: replica handoff apply failed: " << error
                  << '\n';
        return 1;
      }
    }
    std::cout << "goblin-core: replica is live at replication offset "
              << store_.replication_state().offset << '\n';
    store_.set_replica_mode(replica->description());
#ifdef GOBLIN_HAS_KAFKA
    // The firehose connection was established before replay. Once its buffered
    // suffix is applied it is the sole upstream; keeping Kafka active would race
    // the same logical records down two inputs.
    kafka.reset();
#endif
  }

  auto listeners_result = create_socket_listeners(config_);
  if (!listeners_result) {
    return 1;
  }
  auto listeners = std::move(*listeners_result);

  detail::PubSubRegistry pubsub;
  detail::WatchRegistry watches;
  BlockingListRegistry blocking_lists;
  struct StoreObservers {
    detail::WatchRegistry* watches;
    BlockingListRegistry* blocking_lists;
  } store_observers{.watches = &watches, .blocking_lists = &blocking_lists};
  store_.set_mutation_observer(StoreMutationObserver{
      .context = &store_observers,
      .key_modified = [](void* context, std::string_view key) noexcept {
        auto& observers = *static_cast<StoreObservers*>(context);
        observers.watches->modified(key);
        observers.blocking_lists->modified();
      },
      .all_modified = [](void* context) noexcept {
        auto& observers = *static_cast<StoreObservers*>(context);
        observers.watches->modified_all();
        observers.blocking_lists->modified();
      }});
  struct MutationObserverReset {
    Store* store;
    ~MutationObserverReset() { store->set_mutation_observer({}); }
  } mutation_observer_reset{&store_};
  std::vector<std::unique_ptr<Client>> clients;
  running_ = true;

#ifdef GOBLIN_HAS_KAFKA
  ReplicationRuntime replication(kafka_journal.get(),
                                 config_.replication_buffer_bytes);
#else
  ReplicationRuntime replication(config_.replication_buffer_bytes);
#endif
  const NestedCommandDispatch nested_dispatch{
      .context = &pubsub,
      .publish = [](void* context, std::string_view channel,
                    std::string_view payload) {
        return static_cast<detail::PubSubRegistry*>(context)->publish(channel,
                                                                      payload);
      },
      .replication_context = &replication,
      .replicate_write = &ReplicationRuntime::replicate,
      .read_only = replica != nullptr,
  };

  // One engine per interpreter for the process. Each holds its own script cache
  // and, lazily, its own VM -- no VM is created until the first script of that
  // kind runs, so a server that never scripts pays nothing here.
  ScriptEngine script_engine(store_, nested_dispatch);
  LuauEngine luau_engine(store_, nested_dispatch);
  WrenEngine wren_engine(store_, nested_dispatch);
  TclEngine tcl_engine(store_, nested_dispatch);
  UPythonEngine upython_engine(store_, nested_dispatch);
  QuickJsEngine quickjs_engine(store_, nested_dispatch);

  const auto replica_iteration = [&]() -> bool {
    if (!replica) return false;
    ReplicationBatch batch;
    bool available = false;
    std::string error;
    if (!replica->try_next(batch, available, error)) {
      std::cerr << "goblin-core: upstream replication failed: " << error
                << '\n';
      kafka_failed = true;
      running_ = false;
      return true;
    }
    if (!available) return false;
    try {
      if (!replication.apply_received(store_, batch, error)) {
        std::cerr << "goblin-core: upstream replication apply failed: " << error
                  << '\n';
        kafka_failed = true;
        running_ = false;
      }
    } catch (const std::exception& exception) {
      std::cerr << "goblin-core: upstream replication apply failed: "
                << exception.what() << '\n';
      kafka_failed = true;
      running_ = false;
    }
    return true;
  };

  for (const auto& listener : listeners) {
    std::cout << "goblin-core listening on " << listener.description << '\n';
  }

  // Keys expired per active-expiration sweep. A bounded batch keeps a mass
  // expiry from stalling the loop; when a sweep fills the batch more are likely
  // due, so the poll below returns immediately to sweep again.
  constexpr std::size_t kActiveExpireBudget = 1000;
#ifdef GOBLIN_HAS_KAFKA
  constexpr std::size_t kKafkaDrainBudget = 1024;
  bool kafka_may_have_more = kafka && kafka->has_pending();
  const auto poll_kafka_journal = [&]() -> bool {
    if (!kafka_journal) return true;
    std::string error;
    if (kafka_journal->poll(store_, error)) return true;
    std::cerr << "goblin-core: " << error << '\n';
    kafka_failed = true;
    running_ = false;
    return false;
  };
  const auto drain_kafka = [&]() -> bool {
    auto result = kafka->poll(store_, kKafkaDrainBudget);
    if (!result.ok()) {
      std::cerr << "goblin-core: Kafka ingestion failed: " << result.error
                << '\n';
      kafka_failed = true;
      running_ = false;
      return false;
    }
    kafka_may_have_more = result.may_have_more;
    return true;
  };
#endif

  // One pass over the network: reap a finished background save, do a bounded
  // active-expiration sweep, then poll() (blocking up to max_timeout_ms) and
  // service ready clients and new connections. In polled mode this is invoked only
  // when every ring/RDMA target is empty, always with timeout 0 (never blocking).
  const auto network_iteration = [&](int max_timeout_ms) {
    bool replica_progress = false;
    for (std::size_t i = 0; i < 1024 && replica_iteration(); ++i) {
      replica_progress = true;
      if (!running_.load(std::memory_order_relaxed)) return;
    }
#ifdef GOBLIN_HAS_KAFKA
    if (!poll_kafka_journal()) return;
#endif
    if (replication.failed()) {
      std::cerr << "goblin-core: replication failed: "
                << replication.error() << '\n';
      kafka_failed = true;
      running_ = false;
      return;
    }
#ifdef GOBLIN_HAS_KAFKA
    if (kafka && kafka_may_have_more && !drain_kafka()) {
      return;
    }
#endif
    if (auto outcome = store_.reap_background_save()) {
      if (outcome->ok) {
        std::cout << "goblin-core: background save of " << outcome->path
                  << " completed\n"
                  << std::flush;
      } else {
        std::cerr << "goblin-core: background save of " << outcome->path
                  << " FAILED\n";
      }
    }

    int poll_timeout = max_timeout_ms;
    if (replica_progress) poll_timeout = 0;
#ifdef GOBLIN_HAS_KAFKA
    if (kafka_may_have_more) poll_timeout = 0;
#endif
    if (!store_.ttl_empty()) {
      if (store_.active_expire(store_.now_ms(), kActiveExpireBudget) ==
          kActiveExpireBudget) {
        poll_timeout = 0;  // batch full -- more are likely due, sweep again now
      }
    }
    blocking_lists.serve_ready(store_, replication);
    poll_timeout = blocking_lists.clamp_poll_timeout(poll_timeout);

    std::vector<pollfd> pollfds;
    const std::size_t listener_count = listeners.size();
    const std::size_t kafka_fd_count =
#ifdef GOBLIN_HAS_KAFKA
        (kafka ? 1U : 0U) + (kafka_journal ? 1U : 0U);
#else
        0U;
#endif
    const std::size_t replica_fd_count =
        replica && replica->notification_fd() >= 0 ? 1U : 0U;
    pollfds.reserve(clients.size() + listener_count + kafka_fd_count +
                    replica_fd_count);
    for (const auto& listener : listeners) {
      pollfds.push_back(
          pollfd{.fd = listener.fd, .events = POLLIN, .revents = 0});
    }
#ifdef GOBLIN_HAS_KAFKA
    if (kafka) {
      pollfds.push_back(pollfd{.fd = kafka->notification_fd(),
                              .events = POLLIN,
                              .revents = 0});
    }
    if (kafka_journal) {
      pollfds.push_back(pollfd{.fd = kafka_journal->notification_fd(),
                              .events = POLLIN,
                              .revents = 0});
    }
#endif
    if (replica_fd_count != 0) {
      pollfds.push_back(pollfd{.fd = replica->notification_fd(),
                              .events = POLLIN,
                              .revents = 0});
    }
    for (auto& client_ptr : clients) {
      auto& client = *client_ptr;
      update_read_backpressure(client, config_);
      if (!client.read_backpressured && tls_plaintext_pending(client)) {
        poll_timeout = 0;
      }
      pollfds.push_back(pollfd{.fd = client.fd,
                              .events = client_poll_events(client),
                              .revents = 0});
    }

    const int ready = ::poll(pollfds.data(), pollfds.size(), poll_timeout);
    if (ready < 0) {
      if (errno == EINTR) {
        return;
      }
      std::cerr << "goblin-core: poll failed: " << std::strerror(errno) << '\n';
      running_ = false;
      return;
    }

    std::vector<unsigned char> close_client(clients.size(), 0);
#ifdef GOBLIN_HAS_KAFKA
    std::size_t kafka_event_index = listener_count;
    if (kafka) {
      if ((pollfds[kafka_event_index].revents & (POLLIN | POLLERR | POLLHUP)) !=
              0 &&
          !drain_kafka()) {
        return;
      }
      ++kafka_event_index;
    }
    if (kafka_journal &&
        (pollfds[kafka_event_index].revents & (POLLIN | POLLERR | POLLHUP)) !=
            0 &&
        !poll_kafka_journal()) {
      return;
    }
#endif
    const std::size_t replica_event_index = listener_count + kafka_fd_count;
    if (replica_fd_count != 0 &&
        (pollfds[replica_event_index].revents &
         (POLLIN | POLLERR | POLLHUP)) != 0) {
      for (std::size_t i = 0; i < 1024 && replica_iteration(); ++i) {
        if (!running_.load(std::memory_order_relaxed)) return;
      }
    }
    for (std::size_t i = 0; i < clients.size(); ++i) {
      bool keep = !clients[i]->close_requested;
      auto& client = *clients[i];
      const auto revents =
          pollfds[listener_count + kafka_fd_count + replica_fd_count + i]
              .revents;

      if (keep && tls_handshake_ready(client, revents)) {
        keep = advance_tls_handshake(client);
      }
      const bool application_ready = keep && !tls_handshake_pending(client);

      if (application_ready && client_write_ready(client, revents) &&
          has_pending_output(client)) {
        keep = write_client(client, config_);
      }

      if (application_ready && keep && !client.read_backpressured) {
        keep = process_buffered_commands(
            client, store_, pubsub, watches, blocking_lists, script_engine,
            luau_engine,
            wren_engine, tcl_engine, upython_engine, quickjs_engine, config_,
            auth, replication, nested_dispatch);
      }

      if (application_ready && keep && client_read_ready(client, revents) &&
          !client.read_backpressured) {
        keep = read_client(client, store_, pubsub, watches, blocking_lists,
                           script_engine, luau_engine, wren_engine, tcl_engine,
                           upython_engine, quickjs_engine, config_, auth,
                           replication, nested_dispatch);
      }

      if (application_ready && keep && has_pending_output(client) &&
          !tls_read_retry_pending(client) &&
          !tls_write_retry_pending(client)) {
        keep = write_client(client, config_);
      }

      while (application_ready && keep && !client.read_backpressured &&
             has_buffered_work(client)) {
        keep = process_buffered_commands(
            client, store_, pubsub, watches, blocking_lists, script_engine,
            luau_engine,
            wren_engine, tcl_engine, upython_engine, quickjs_engine, config_,
            auth, replication, nested_dispatch);
        if (keep && has_pending_output(client) &&
            !tls_read_retry_pending(client) &&
            !tls_write_retry_pending(client)) {
          keep = write_client(client, config_);
        }
      }

      if (keep && (revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 &&
          !has_pending_output(client)) {
        keep = false;
      }

      if (!keep) {
        close_client[i] = 1;
      }
    }

    for (std::size_t i = clients.size(); i > 0; --i) {
      const auto index = i - 1;
      if (close_client[index] != 0) {
        replication.remove(*clients[index]);
        pubsub.remove(*clients[index]);
        watches.remove(clients[index]->transaction);
        blocking_lists.remove(*clients[index]);
        close_fd(clients[index]->fd);
        clients.erase(clients.begin() + static_cast<long>(index));
      }
    }

    for (std::size_t i = 0; i < listener_count; ++i) {
      if ((pollfds[i].revents & POLLIN) != 0) {
        void* listener_tls_context = nullptr;
#ifdef GOBLIN_HAS_TLS
        if (listeners[i].tls) {
          listener_tls_context = tls_context.get();
        }
#endif
        accept_clients(listeners[i].fd, listeners[i].tcp, clients, config_,
                       listener_tls_context);
      }
    }
  };

  if (config_.poll_targets.empty() && !config_.pubsub_listener &&
      !(replica && replica->polled())) {
    // No polled targets: the ordinary event-driven server. poll() blocks, so an idle
    // server costs no CPU.
    while (running_) {
      network_iteration(1000);
    }
  } else if (!run_polled_targets(config_, store_, running_, pubsub, watches,
                                 blocking_lists, script_engine, luau_engine,
                                 wren_engine,
                                 tcl_engine, upython_engine, quickjs_engine,
                                 auth, replication, nested_dispatch,
                                 replica_iteration, network_iteration)) {
    close_socket_listeners(listeners);
    return 1;
  }

  for (const auto& client : clients) {
    replication.remove(*client);
    pubsub.remove(*client);
    watches.remove(client->transaction);
    blocking_lists.remove(*client);
    close_fd(client->fd);
  }
  close_socket_listeners(listeners);

  return kafka_failed ? 1 : 0;
}

}  // namespace goblin::core
