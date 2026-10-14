#include "goblin/core/simd_ops.hpp"
#include "goblin/core/snapshot.hpp"

#undef NDEBUG
#include <algorithm>
#include <array>
#include <cassert>
#include <iostream>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>

namespace simd = goblin::core::simd;

namespace {
constexpr std::array models{simd::Model::scalar, simd::Model::sse2,
    simd::Model::avx2, simd::Model::avx512, simd::Model::neon,
    simd::Model::lsx, simd::Model::lasx};

std::optional<std::size_t> position(std::size_t value) {
  return value == std::string::npos ? std::nullopt
                                  : std::optional<std::size_t>{value};
}

// Independent bit-by-bit reference checks both hardware and software CRCs.
std::uint32_t reference_crc(std::string_view data) {
  std::uint32_t crc = 0xFFFFFFFFu;
  for (const unsigned char byte : data) {
    crc ^= byte;
    for (int i = 0; i < 8; ++i)
      crc = (crc & 1) ? (crc >> 1) ^ 0x82F63B78u : crc >> 1;
  }
  return crc ^ 0xFFFFFFFFu;
}

void check_backend() {
  const auto width = simd::fingerprint_group_width();
  assert(width == 16 || width == 32 || width == 64);
  std::mt19937 random{1234};
  // Allocate exactly the bytes each probe is allowed to read, unaligned.
  auto bytes = std::make_unique<std::uint8_t[]>(width + 1);
  auto hashes = std::make_unique<std::uint32_t[]>(17);
  for (int trial = 0; trial < 100; ++trial) {
    const auto needle = static_cast<std::uint8_t>(random());
    std::uint64_t byte_mask = 0;
    for (std::size_t i = 0; i < width; ++i) {
      bytes[i + 1] = random() % 3 == 0 ? needle
                                     : static_cast<std::uint8_t>(random());
      byte_mask |= std::uint64_t{bytes[i + 1] == needle} << i;
    }
    assert(simd::match_control_group_16(bytes.get() + 1, needle) ==
           (byte_mask & 0xFFFFu));
    assert(simd::match_fingerprint_group(bytes.get() + 1, needle) == byte_mask);
    const std::uint32_t hash = random();
    std::uint64_t hash_mask = 0;
    for (std::size_t i = 0; i < 16; ++i) {
      hashes[i + 1] = random() % 3 == 0 ? hash : random();
      hash_mask |= std::uint64_t{hashes[i + 1] == hash} << i;
    }
    assert(simd::match_hash_group_16(hashes.get() + 1, hash) == hash_mask);
  }
  assert(simd::bytes_equal_n(nullptr, nullptr, 0));
  assert(!simd::bytes_equal("a", "aa"));
  assert(simd::find_crlf({}, 0) == std::nullopt);
  assert(simd::find_byte({}, 'a', 0) == std::nullopt);
  assert(goblin::core::snapshot::checksum("123456789") == 0xE3069283u);

  // All lengths around vector boundaries, each possible mismatch, and scans
  // starting partway through a vector. Include lone CR and cross-vector CRLF.
  for (std::size_t length = 0; length <= 193; ++length) {
    std::string text(length, 'x');
    for (auto& byte : text) byte = static_cast<char>(random());
    assert(simd::checksum(text) == reference_crc(text));
    // Exact allocations expose overreads to ASan, with a one-byte misalignment.
    auto lhs = std::make_unique<char[]>(length + 1);
    auto rhs = std::make_unique<char[]>(length + 1);
    std::copy(text.begin(), text.end(), lhs.get() + 1);
    std::copy(text.begin(), text.end(), rhs.get() + 1);
    assert(simd::bytes_equal_n(lhs.get() + 1, rhs.get() + 1, length));
    for (std::size_t at = 0; at < length; ++at) {
      rhs[at + 1] ^= 1;
      assert(!simd::bytes_equal_n(lhs.get() + 1, rhs.get() + 1, length));
      rhs[at + 1] ^= 1;
    }
    for (std::size_t at = 0; at < length; ++at) {
      std::string scan(length, 'x');
      scan[at] = '\r';
      if (at + 1 < length) scan[at + 1] = '\n';
      if (at > 1) scan[at - 2] = '\r';
      for (std::size_t offset = 0; offset <= length + 1; ++offset) {
        const std::string_view view(scan);
        assert(simd::find_byte(view, '\r', offset) ==
               position(view.find('\r', offset)));
        assert(simd::find_crlf(view, offset) ==
               position(view.find("\r\n", offset)));
      }
    }
  }

  for (std::size_t n = 0; n <= 35; ++n) {
    std::array<std::uint64_t, 38> dst{}, src{}, expected{};
    for (std::size_t i = 0; i < dst.size(); ++i) {
      dst[i] = (std::uint64_t{random()} << 32) | random();
      src[i] = (std::uint64_t{random()} << 32) | random();
      expected[i] = dst[i];
    }
    for (std::size_t i = 1; i <= n; ++i) expected[i] |= src[i];
    simd::or_u64_words(dst.data() + 1, src.data() + 1, n);
    assert(dst == expected);  // Includes sentinels outside the requested range.
    simd::or_u64_words(dst.data() + 1, dst.data() + 1, n);
    assert(dst == expected);
  }
}
}  // namespace

int main() {
  simd::configure();
  const auto automatic = simd::selected_model();
  assert(simd::is_supported(automatic));
  // Hash identity must survive backend changes, including XXH3's long-input
  // path (>240 bytes); otherwise existing hash tables become unsearchable.
  std::string hash_input(4097, '\0');
  std::mt19937 random{5678};
  for (auto& byte : hash_input) byte = static_cast<char>(random());
  std::array<std::uint64_t, 4097> hashes{};
  simd::configure("scalar");
  for (std::size_t n = 0; n < hashes.size(); ++n)
    hashes[n] = simd::hash_bytes(hash_input.data() + 1, n);
  for (const auto model : models) {
    if (!simd::is_supported(model)) {
      const auto previous = simd::selected_model();
      try {
        simd::configure(simd::model_name(model));
        assert(false);
      } catch (const std::invalid_argument&) {
        assert(simd::selected_model() == previous);
      }
      continue;
    }
    simd::configure(simd::model_name(model));
    assert(simd::selected_model() == model);
    for (std::size_t n = 0; n < hashes.size(); ++n)
      assert(simd::hash_bytes(hash_input.data() + 1, n) == hashes[n]);
    check_backend();
    std::cout << "verified " << simd::model_name(model) << '\n';
  }
  const auto previous = simd::selected_model();
  for (const auto name : {"", "invalid", "AVX2"}) {
    try {
      simd::configure(name);
      assert(false);
    } catch (const std::invalid_argument&) {
      assert(simd::selected_model() == previous);
    }
  }
  simd::configure("auto");
  assert(simd::selected_model() == automatic);
}
