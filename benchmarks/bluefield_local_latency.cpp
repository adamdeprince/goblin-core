#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <initializer_list>
#include <limits>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

inline void cpu_relax() noexcept {
#if defined(__x86_64__) || defined(__i386__)
  __builtin_ia32_pause();
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield");
#else
  __asm__ __volatile__("" ::: "memory");
#endif
}

class Socket {
 public:
  Socket(std::string_view host, std::uint16_t port, bool spin)
      : spin_(spin) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* addresses = nullptr;
    const std::string host_text(host);
    const std::string port_text = std::to_string(port);
    const int result = ::getaddrinfo(host_text.c_str(), port_text.c_str(),
                                     &hints, &addresses);
    if (result != 0) {
      throw std::runtime_error(std::string("getaddrinfo: ") +
                               ::gai_strerror(result));
    }
    for (auto* address = addresses; address != nullptr;
         address = address->ai_next) {
      fd_ = ::socket(address->ai_family, address->ai_socktype,
                     address->ai_protocol);
      if (fd_ < 0) continue;
      int enabled = 1;
      (void)::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &enabled,
                         sizeof(enabled));
      if (::connect(fd_, address->ai_addr,
                    static_cast<socklen_t>(address->ai_addrlen)) == 0) {
        break;
      }
      (void)::close(fd_);
      fd_ = -1;
    }
    ::freeaddrinfo(addresses);
    if (fd_ < 0) {
      throw std::runtime_error("could not connect to the BlueField edge");
    }
  }

  ~Socket() {
    if (fd_ >= 0) (void)::close(fd_);
  }

  Socket(const Socket&) = delete;
  Socket& operator=(const Socket&) = delete;

  void send_all(std::string_view bytes) {
    while (!bytes.empty()) {
      const ssize_t sent = ::send(fd_, bytes.data(), bytes.size(), 0);
      if (sent > 0) {
        bytes.remove_prefix(static_cast<std::size_t>(sent));
      } else if (sent < 0 && errno == EINTR) {
        continue;
      } else if (sent < 0 && spin_ &&
                 (errno == EAGAIN || errno == EWOULDBLOCK)) {
        cpu_relax();
      } else {
        throw std::runtime_error("send: " + std::string(std::strerror(errno)));
      }
    }
  }

  void expect(std::string_view expected) {
    std::size_t offset = 0;
    while (offset < expected.size()) {
      char buffer[4096];
      const std::size_t want =
          std::min<std::size_t>(sizeof(buffer), expected.size() - offset);
      const ssize_t received =
          ::recv(fd_, buffer, want, spin_ ? MSG_DONTWAIT : 0);
      if (received > 0) {
        const auto count = static_cast<std::size_t>(received);
        if (std::memcmp(buffer, expected.data() + offset, count) != 0) {
          throw std::runtime_error("unexpected RESP reply from the edge");
        }
        offset += count;
      } else if (received < 0 && errno == EINTR) {
        continue;
      } else if (received < 0 && spin_ &&
                 (errno == EAGAIN || errno == EWOULDBLOCK)) {
        cpu_relax();
      } else if (received == 0) {
        throw std::runtime_error("edge closed the benchmark connection");
      } else {
        throw std::runtime_error("recv: " + std::string(std::strerror(errno)));
      }
    }
  }

 private:
  int fd_{-1};
  bool spin_{false};
};

void append_bulk(std::string& out, std::string_view value) {
  out.push_back('$');
  out.append(std::to_string(value.size()));
  out.append("\r\n");
  out.append(value);
  out.append("\r\n");
}

std::string command(std::initializer_list<std::string_view> fields) {
  std::string out = "*" + std::to_string(fields.size()) + "\r\n";
  for (const auto field : fields) append_bulk(out, field);
  return out;
}

std::string message(std::string_view channel, std::string_view payload) {
  std::string out("*3\r\n");
  append_bulk(out, "message");
  append_bulk(out, channel);
  append_bulk(out, payload);
  return out;
}

std::string subscribe_ack(std::string_view channel) {
  std::string out("*3\r\n");
  append_bulk(out, "subscribe");
  append_bulk(out, channel);
  out.append(":1\r\n");
  return out;
}

std::uint16_t parse_port(std::string_view text) {
  unsigned value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
      value == 0 || value > std::numeric_limits<std::uint16_t>::max()) {
    throw std::runtime_error("invalid port");
  }
  return static_cast<std::uint16_t>(value);
}

std::size_t parse_count(std::string_view text) {
  std::size_t value = 0;
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    throw std::runtime_error("invalid iteration count");
  }
  return value;
}

double percentile(const std::vector<double>& sorted, double fraction) {
  const auto raw = static_cast<std::size_t>(
      std::ceil(fraction * static_cast<double>(sorted.size())));
  return sorted[std::max<std::size_t>(raw, 1) - 1];
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || argc > 7) {
    std::cerr << "usage: bluefield-local-latency HOST PORT [ITERATIONS] "
                 "[WARMUP] [CHANNEL] [blocking|spin]\n";
    return 2;
  }
  try {
    const std::string_view host(argv[1]);
    const auto port = parse_port(argv[2]);
    const std::size_t iterations = argc >= 4 ? parse_count(argv[3]) : 1000;
    const std::size_t warmup = argc >= 5 ? parse_count(argv[4]) : 100;
    const std::string channel =
        argc >= 6 ? argv[5] : "benchmark:bluefield:native";
    const std::string_view wait_mode = argc >= 7 ? argv[6] : "blocking";
    if (wait_mode != "blocking" && wait_mode != "spin") {
      throw std::runtime_error("wait mode must be blocking or spin");
    }
    const bool spin = wait_mode == "spin";
    if (iterations == 0) throw std::runtime_error("iterations must be positive");

    Socket subscriber(host, port, spin);
    Socket publisher(host, port, spin);
    subscriber.send_all(command({"SUBSCRIBE", channel}));
    subscriber.expect(subscribe_ack(channel));

    std::vector<double> microseconds;
    microseconds.reserve(iterations);
    for (std::size_t index = 0; index < warmup + iterations; ++index) {
      const std::string payload = "p" + std::to_string(index);
      const std::string publish = command({"PUBLISH", channel, payload});
      const std::string push = message(channel, payload);
      const auto before = Clock::now();
      publisher.send_all(publish);
      subscriber.expect(push);
      const auto after = Clock::now();
      // One local subscriber and no non-edge subscribers. The host excludes the
      // origin aggregate, so the combined Redis PUBLISH count is exactly one.
      publisher.expect(":1\r\n");
      if (index >= warmup) {
        microseconds.push_back(
            std::chrono::duration<double, std::micro>(after - before).count());
      }
    }

    std::sort(microseconds.begin(), microseconds.end());
    double sum = 0;
    for (const double value : microseconds) sum += value;
    std::cout << "mode=" << wait_mode << " samples=" << microseconds.size()
              << " min=" << microseconds.front()
              << " p50=" << percentile(microseconds, 0.50)
              << " p90=" << percentile(microseconds, 0.90)
              << " p99=" << percentile(microseconds, 0.99)
              << " max=" << microseconds.back()
              << " mean=" << sum / static_cast<double>(microseconds.size())
              << " us\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "bluefield-local-latency: " << error.what() << '\n';
    return 1;
  }
}
