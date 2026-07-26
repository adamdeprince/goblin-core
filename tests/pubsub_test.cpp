#include "../src/pubsub.hpp"

#include <string>
#include <string_view>
#include <vector>

#undef NDEBUG
#include <cassert>

using goblin::core::detail::PubSubRegistry;
using goblin::core::detail::PubSubSession;
using goblin::core::detail::UnsolicitedOutputQueue;

int main() {
  assert(PubSubRegistry::glob_match("*", "anything"));
  assert(PubSubRegistry::glob_match("h?llo", "hello"));
  assert(PubSubRegistry::glob_match("h[ae]llo", "hello"));
  assert(PubSubRegistry::glob_match("h[a-c]llo", "hbllo"));
  assert(PubSubRegistry::glob_match("h[^e]llo", "hallo"));
  assert(!PubSubRegistry::glob_match("h[^e]llo", "hello"));
  assert(PubSubRegistry::glob_match(R"(literal\*)", "literal*"));
  assert(!PubSubRegistry::glob_match(R"(literal\*)", "literal-value"));

  // Fast-path classification for common shapes.
  assert(PubSubRegistry::classify_pattern("*") ==
         goblin::core::detail::PatternKind::always);
  assert(PubSubRegistry::classify_pattern("news") ==
         goblin::core::detail::PatternKind::literal);
  assert(PubSubRegistry::classify_pattern("news*") ==
         goblin::core::detail::PatternKind::prefix);
  assert(PubSubRegistry::classify_pattern("*alerts") ==
         goblin::core::detail::PatternKind::suffix);
  assert(PubSubRegistry::classify_pattern("a*b") ==
         goblin::core::detail::PatternKind::prefix_suffix);
  assert(PubSubRegistry::classify_pattern("h?llo") ==
         goblin::core::detail::PatternKind::general);
  assert(PubSubRegistry::match_classified(
      goblin::core::detail::PatternKind::prefix, "news*", "news.sports"));
  assert(!PubSubRegistry::match_classified(
      goblin::core::detail::PatternKind::prefix, "news*", "old.news"));
  assert(PubSubRegistry::match_classified(
      goblin::core::detail::PatternKind::suffix, "*alerts", "crit.alerts"));
  assert(PubSubRegistry::match_classified(
      goblin::core::detail::PatternKind::prefix_suffix, "a*b", "a--b"));
  assert(!PubSubRegistry::match_classified(
      goblin::core::detail::PatternKind::prefix_suffix, "a*b", "abx"));

  // Redis character classes only use '^' for negation. '!' is a member.
  assert(PubSubRegistry::glob_match("x[!a]", "x!"));
  assert(PubSubRegistry::glob_match("x[!a]", "xa"));
  assert(!PubSubRegistry::glob_match("x[!a]", "xb"));

  const std::string binary_pattern{"a?c", 3};
  const std::string binary_value{"a\0c", 3};
  assert(PubSubRegistry::glob_match(binary_pattern, binary_value));

  UnsolicitedOutputQueue queue(128);
  assert(queue.mapped_bytes() == 128);
  assert(queue.push(1, std::string(30, 'a')));
  assert(queue.push(2, std::string(30, 'b')));
  assert(queue.payload_bytes() == 60);
  assert(queue.front()->sequence == 1);
  assert(queue.front()->bytes == std::string(30, 'a'));
  queue.pop_front(30);

  assert(queue.push(3, std::string(25, 'c')));
  assert(queue.front()->sequence == 2);
  queue.pop();  // re-reads length; keeps the older API honest

  // This record cannot fit at the seven-byte tail, so it wraps to offset zero.
  assert(queue.push(4, std::string(20, 'd')));
  assert(queue.front()->sequence == 3);
  queue.pop_front(25);
  assert(queue.front()->sequence == 4);
  assert(queue.front()->bytes == std::string(20, 'd'));
  queue.pop_front(20);
  assert(queue.empty());
  assert(queue.used_bytes() == 0);
  assert(queue.payload_bytes() == 0);

  UnsolicitedOutputQueue exact(64);
  assert(exact.push(9, std::string(52, 'x')));
  assert(!exact.push(10, {}));
  assert(exact.used_bytes() == 64);
  exact.clear();
  assert(exact.push(10, {}));
  assert(exact.front()->sequence == 10);
  assert(exact.front()->bytes.empty());
  exact.pop();
  assert(!exact.front().has_value());

  // Unsubscribe-all drains the batch-cleared reverse-name index. A second
  // subscriber on one channel must survive the subscription-table cleanup.
  PubSubRegistry registry;
  PubSubSession bulk_session(4096);
  PubSubSession survivor(4096);
  constexpr std::size_t kChannels = 4096;
  std::vector<std::string> names;
  std::vector<std::string_view> name_views;
  names.reserve(kChannels);
  name_views.reserve(kChannels);
  for (std::size_t index = 0; index < kChannels; ++index) {
    names.push_back("channel-" + std::to_string(index));
  }
  for (const auto& name : names) {
    name_views.push_back(name);
  }

  std::string output;
  registry.execute(
      bulk_session,
      {.type = goblin::core::CommandType::subscribe, .args = name_views},
      output);
  const std::string_view shared_channel = names[kChannels / 2];
  registry.execute(
      survivor,
      {.type = goblin::core::CommandType::subscribe,
       .args = std::span(&shared_channel, 1)},
      output);
  assert(bulk_session.literal_subscriptions == kChannels);
  assert(bulk_session.channel_names.size() == kChannels);

  output.clear();
  registry.execute(
      bulk_session,
      {.type = goblin::core::CommandType::unsubscribe, .args = {}}, output);
  assert(bulk_session.literal_subscriptions == 0);
  assert(bulk_session.channel_names.empty());
  assert(registry.publish(names.front(), "payload") == 0);
  assert(registry.publish(shared_channel, "payload") == 1);

  registry.remove(survivor);
  assert(registry.publish(shared_channel, "payload") == 0);
}
