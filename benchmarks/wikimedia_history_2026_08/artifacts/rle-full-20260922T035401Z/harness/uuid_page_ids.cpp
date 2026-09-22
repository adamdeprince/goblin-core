// Convert the numeric Wikimedia replay to order-preserving UUID page IDs.
// The input and output must differ; O_EXCL refuses to overwrite any output.
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char** argv) {
  if (argc != 3) {
    std::fprintf(stderr, "usage: uuid-page-ids NUMERIC_INPUT NEW_UUID_OUTPUT\n");
    return 2;
  }
  try {
    const int input = open(argv[1], O_RDONLY);
    if (input < 0) throw std::runtime_error("cannot open input");
    struct stat st {};
    if (fstat(input, &st) != 0 || st.st_size <= 0) {
      throw std::runtime_error("cannot stat nonempty input");
    }
    const auto bytes = static_cast<std::size_t>(st.st_size);
    const auto* data = static_cast<const char*>(
        mmap(nullptr, bytes, PROT_READ, MAP_PRIVATE, input, 0));
    if (data == MAP_FAILED) throw std::runtime_error("cannot map input");
    const int output = open(argv[2], O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (output < 0) throw std::runtime_error("cannot create new output");
    std::vector<char> buffer(16 * 1024 * 1024);
    std::size_t used = 0;
    const auto flush = [&] {
      std::size_t offset = 0;
      while (offset < used) {
        const auto written = write(output, buffer.data() + offset, used - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) throw std::runtime_error("output write failed");
        offset += static_cast<std::size_t>(written);
      }
      used = 0;
    };
    constexpr char prefix[] = "ZINCRBY key 1 ";
    char line[] = "ZINCRBY key 1 00000000-0000-0000-0000-000000000000\n";
    constexpr std::size_t line_bytes = sizeof(line) - 1;
    static_assert(line_bytes == 51);
    constexpr char hex[] = "0123456789abcdef";
    std::uint64_t commands = 0;
    std::uint32_t maximum = 0;
    std::uint32_t minimum = std::numeric_limits<std::uint32_t>::max();
    const auto* cursor = data;
    const auto* end = data + bytes;
    while (cursor < end) {
      const auto* newline = static_cast<const char*>(
          std::memchr(cursor, '\n', std::min<std::size_t>(128, end - cursor)));
      if (newline == nullptr || static_cast<std::size_t>(newline - cursor) <= sizeof(prefix) - 1 ||
          std::memcmp(cursor, prefix, sizeof(prefix) - 1) != 0) {
        throw std::runtime_error("unexpected replay command at line " +
                                 std::to_string(commands + 1));
      }
      std::uint32_t id = 0;
      const auto parsed = std::from_chars(cursor + sizeof(prefix) - 1, newline, id);
      if (parsed.ec != std::errc{} || parsed.ptr != newline || id > 0x7fffffff) {
        throw std::runtime_error("invalid INT32 page ID");
      }
      maximum = std::max(maximum, id);
      minimum = std::min(minimum, id);
      for (std::size_t nibble = 0; nibble < 8; ++nibble) {
        line[line_bytes - 2 - nibble] = hex[(id >> (nibble * 4)) & 15];
      }
      if (buffer.size() - used < line_bytes) flush();
      std::memcpy(buffer.data() + used, line, line_bytes);
      used += line_bytes;
      ++commands;
      cursor = newline + 1;
    }
    flush();
    if (close(output) != 0) throw std::runtime_error("output close failed");
    munmap(const_cast<char*>(data), bytes);
    close(input);
    std::printf("{\"commands\":%llu,\"input_bytes\":%zu,\"output_bytes\":%llu,"
                "\"min_page_id\":%u,\"max_page_id\":%u}\n",
                static_cast<unsigned long long>(commands), bytes,
                static_cast<unsigned long long>(commands * line_bytes), minimum,
                maximum);
    return 0;
  } catch (const std::exception& error) {
    std::fprintf(stderr, "uuid-page-ids: %s\n", error.what());
    return 1;
  }
}
