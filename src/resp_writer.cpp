#include "goblin/core/resp_writer.hpp"

#include <string>

namespace goblin::core::resp {

std::string simple_string(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 3);
  append_simple_string(out, value);
  return out;
}

std::string error(std::string_view message) {
  std::string out;
  out.reserve(message.size() + 3);
  append_error(out, message);
  return out;
}

std::string integer(long long value) {
  std::string out;
  out.reserve(24);
  append_integer(out, value);
  return out;
}

std::string bulk_string(std::string_view value) {
  std::string out;
  out.reserve(value.size() + 32);
  append_bulk_string(out, value);
  return out;
}

std::string null_bulk_string() {
  std::string out;
  out.reserve(5);
  append_null_bulk_string(out);
  return out;
}

std::string array(std::span<const std::string_view> values) {
  std::string out;
  out.reserve(16);
  append_array_header(out, values.size());

  for (const auto& value : values) {
    append_bulk_string(out, value);
  }

  return out;
}

}  // namespace goblin::core::resp
