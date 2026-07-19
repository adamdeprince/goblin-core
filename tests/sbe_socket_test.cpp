// End-to-end SBE over a UNIX socket, across two processes: fork+exec the server with
// a --unixsocket, connect, send the GOBLINS! magic once, then framed SBE requests,
// and decode the framed replies. Proves the socket accept path detects the magic and
// dispatches SBE (not just the shared-memory ring).
//
//   sbe_socket_test <path-to-goblin-core>

#include "goblin/core/auth.hpp"
#include "goblin/core/goblin_protocol.hpp"
#include "goblin/core/sbe_frame.hpp"

#include "goblin_sbe/IntReply.h"
#include "goblin_sbe/MessageHeader.h"
#include "goblin_sbe/Ping.h"
#include "goblin_sbe/PubSubPush.h"
#include "goblin_sbe/Publish.h"
#include "goblin_sbe/StatusReply.h"
#include "goblin_sbe/Subscribe.h"
#include "goblin_sbe/ZAdd.h"

#include <csignal>  // kill / SIGTERM (not transitively included via <sys/wait.h> on macOS)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#undef NDEBUG
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <thread>

using namespace goblin::core;

namespace {

// Read one length-prefixed SBE reply frame into `frame` (message bytes, prefix
// stripped); returns the templateId.
std::uint16_t read_frame(int fd, std::string& acc, std::string& frame) {
  for (;;) {
    if (acc.size() >= kSbeLenPrefix) {
      std::uint32_t len = 0;
      std::memcpy(&len, acc.data(), kSbeLenPrefix);
      if (acc.size() >= kSbeLenPrefix + len) {
        frame.assign(acc.data() + kSbeLenPrefix, len);
        acc.erase(0, kSbeLenPrefix + len);
        goblin_sbe::MessageHeader hdr(frame.data(), frame.size());
        return hdr.templateId();
      }
    }
    char buf[4096];
    const ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
    assert(n > 0 && "server closed or errored before a full reply");
    acc.append(buf, static_cast<std::size_t>(n));
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: sbe_socket_test <goblin-core>\n"); return 2; }
  const char* server = argv[1];
  const std::string suffix = std::to_string(::getpid());
  const std::string sock = "/tmp/gcsbe-sock-" + suffix + ".sock";
  const std::string auth = "/tmp/gcsbe-auth-" + suffix + ".conf";
  ::unlink(sock.c_str());
  ::unlink(auth.c_str());
  ::unlink((auth + ".lock").c_str());
  assert(goblin::core::upsert_auth_user(auth, "default", "secret") ==
         goblin::core::AuthUserUpdate::added);

  const pid_t pid = ::fork();
  assert(pid >= 0);
  if (pid == 0) {
    const int dn = ::open("/dev/null", O_WRONLY);
    if (dn >= 0) { ::dup2(dn, 1); ::dup2(dn, 2); }
    ::execl(server, server, "--enable-sbe", "--auth-file", auth.c_str(),
            "--unixsocket", sock.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  assert(fd >= 0);
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);
  bool connected = false;
  for (int i = 0; i < 500; ++i) {
    if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) { connected = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  assert(connected && "could not connect to the server socket");

  // Switch this connection to SBE: the magic, once.
  assert(::send(fd, kGoblinMagicBytes, sizeof(kGoblinMagicBytes), 0) ==
         static_cast<ssize_t>(sizeof(kGoblinMagicBytes)));

  std::string acc;   // reply byte accumulator
  std::string frame; // one decoded reply message

  // PING -> StatusReply "PONG"
  {
    char buf[64];
    goblin_sbe::Ping p;
    p.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
    const std::uint32_t total = static_cast<std::uint32_t>(goblin_sbe::Ping::sbeBlockAndHeaderLength());
    std::memcpy(buf, &total, kSbeLenPrefix);
    assert(::send(fd, buf, kSbeLenPrefix + total, 0) > 0);

    const auto tid = read_frame(fd, acc, frame);
    assert(tid == goblin_sbe::StatusReply::sbeTemplateId());
    goblin_sbe::MessageHeader hdr(frame.data(), frame.size());
    goblin_sbe::StatusReply r;
    r.wrapForDecode(frame.data(), goblin_sbe::MessageHeader::encodedLength(), hdr.blockLength(),
                    hdr.version(), frame.size());
    assert(r.getStatusAsStringView() == "PONG");
  }

  // ZADD board 1.5 alice -> IntReply 1  (native double straight through)
  {
    char buf[128];
    goblin_sbe::ZAdd z;
    z.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
    z.flags(0);
    z.membersCount(1).next().score(1.5).putMember("alice", 5);
    z.putKey("board", 5);
    const std::uint32_t total =
        static_cast<std::uint32_t>(goblin_sbe::MessageHeader::encodedLength() + z.encodedLength());
    std::memcpy(buf, &total, kSbeLenPrefix);
    assert(::send(fd, buf, kSbeLenPrefix + total, 0) > 0);

    const auto tid = read_frame(fd, acc, frame);
    assert(tid == goblin_sbe::IntReply::sbeTemplateId());
    goblin_sbe::MessageHeader hdr(frame.data(), frame.size());
    goblin_sbe::IntReply r;
    r.wrapForDecode(frame.data(), goblin_sbe::MessageHeader::encodedLength(), hdr.blockLength(),
                    hdr.version(), frame.size());
    assert(r.value() == 1);
  }

  // Typed Pub/Sub stays available over sockets. A self-publication puts the
  // asynchronous push ahead of the Publish IntReply on the same byte stream.
  {
    char buf[128];
    goblin_sbe::Subscribe subscribe;
    subscribe.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
    subscribe.channelsCount(1).next().putChannel("ticks", 5);
    const auto total = static_cast<std::uint32_t>(
        goblin_sbe::MessageHeader::encodedLength() + subscribe.encodedLength());
    std::memcpy(buf, &total, kSbeLenPrefix);
    assert(::send(fd, buf, kSbeLenPrefix + total, 0) > 0);

    assert(read_frame(fd, acc, frame) == goblin_sbe::PubSubPush::sbeTemplateId());
    goblin_sbe::MessageHeader ack_header(frame.data(), frame.size());
    goblin_sbe::PubSubPush ack;
    ack.wrapForDecode(frame.data(), goblin_sbe::MessageHeader::encodedLength(),
                      ack_header.blockLength(), ack_header.version(), frame.size());
    assert(ack.kind() == 2);
    assert(ack.subscriptionCount() == 1);
    assert(ack.getPatternAsStringView().empty());
    assert(ack.getChannelAsStringView() == "ticks");
  }

  {
    char buf[128];
    goblin_sbe::Publish publish;
    publish.wrapAndApplyHeader(buf, kSbeLenPrefix, sizeof(buf));
    publish.putChannel("ticks", 5);
    publish.putPayload("101.25", 6);
    const auto total = static_cast<std::uint32_t>(
        goblin_sbe::MessageHeader::encodedLength() + publish.encodedLength());
    std::memcpy(buf, &total, kSbeLenPrefix);
    assert(::send(fd, buf, kSbeLenPrefix + total, 0) > 0);

    assert(read_frame(fd, acc, frame) == goblin_sbe::PubSubPush::sbeTemplateId());
    goblin_sbe::MessageHeader push_header(frame.data(), frame.size());
    goblin_sbe::PubSubPush push;
    push.wrapForDecode(frame.data(), goblin_sbe::MessageHeader::encodedLength(),
                       push_header.blockLength(), push_header.version(), frame.size());
    assert(push.kind() == 0);
    assert(push.getPatternAsStringView().empty());
    assert(push.getChannelAsStringView() == "ticks");
    assert(push.getPayloadAsStringView() == "101.25");

    assert(read_frame(fd, acc, frame) == goblin_sbe::IntReply::sbeTemplateId());
    goblin_sbe::MessageHeader reply_header(frame.data(), frame.size());
    goblin_sbe::IntReply reply;
    reply.wrapForDecode(frame.data(), goblin_sbe::MessageHeader::encodedLength(),
                        reply_header.blockLength(), reply_header.version(),
                        frame.size());
    assert(reply.value() == 1);
  }

  ::close(fd);
  ::kill(pid, SIGTERM);
  ::waitpid(pid, nullptr, 0);
  ::unlink(sock.c_str());
  ::unlink(auth.c_str());
  ::unlink((auth + ".lock").c_str());
  std::puts("sbe socket OK: GOBLINS! magic over a UNIX socket -> SBE dispatch and Pub/Sub");
  return 0;
}
