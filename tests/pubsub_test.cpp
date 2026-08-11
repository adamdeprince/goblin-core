#include "../src/pubsub.hpp"

#include <string>
#include <string_view>
#include <vector>

#undef NDEBUG
#include <cassert>

using goblin::core::detail::PubSubRegistry;
using goblin::core::detail::PubSubSession;
using goblin::core::detail::UnsolicitedOutputQueue;
using goblin::core::detail::WireMode;

#ifdef GOBLIN_BLUEFIELD_STANDALONE
struct DirectWriteProbe {
  std::size_t limit{0};
  std::size_t calls{0};
  bool fatal{false};
  std::string bytes;
};
#endif

struct SubscriptionChange {
  std::string name;
  bool pattern{false};
  std::size_t subscribers{0};
};

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

  // Edge observers receive every cardinality transition, not just first/last,
  // so the host can preserve Redis subscriber counts with one aggregate link.
  PubSubRegistry observed;
  std::vector<SubscriptionChange> changes;
  observed.set_subscription_observer({
      .context = &changes,
      .changed = [](void* context, std::string_view name, bool pattern,
                    std::size_t subscribers) {
        static_cast<std::vector<SubscriptionChange>*>(context)->push_back(
            {.name = std::string(name),
             .pattern = pattern,
             .subscribers = subscribers});
      }});
  PubSubSession observed_a(4096);
  PubSubSession observed_b(4096);
  const std::string_view observed_channel = "observed";
  output.clear();
  observed.execute(
      observed_a,
      {.type = goblin::core::CommandType::subscribe,
       .args = std::span(&observed_channel, 1)},
      output);
  observed.execute(
      observed_b,
      {.type = goblin::core::CommandType::subscribe,
       .args = std::span(&observed_channel, 1)},
      output);
  observed.execute(
      observed_a,
      {.type = goblin::core::CommandType::unsubscribe,
       .args = std::span(&observed_channel, 1)},
      output);
  observed.remove(observed_b);
  assert(changes.size() == 4);
  assert(changes[0].name == observed_channel && changes[0].subscribers == 1);
  assert(changes[1].subscribers == 2);
  assert(changes[2].subscribers == 1);
  assert(changes[3].subscribers == 0);

#ifdef GOBLIN_BLUEFIELD_STANDALONE
  // The DPU fast writer may consume a prefix directly. The exact remainder
  // must retain the same output sequence in the mmap backpressure queue.
  PubSubRegistry direct;
  DirectWriteProbe direct_probe{.limit = 7};
  direct.set_direct_writer({
      .context = &direct_probe,
      .write = [](void* context, PubSubSession&,
                  std::string_view bytes) {
        auto& probe = *static_cast<DirectWriteProbe*>(context);
        ++probe.calls;
        if (probe.fatal) {
          return goblin::core::detail::PubSubDirectWriteResult{
              .consumed = 0, .fatal = true};
        }
        const std::size_t consumed =
            probe.limit < bytes.size() ? probe.limit : bytes.size();
        probe.bytes.append(bytes.substr(0, consumed));
        return goblin::core::detail::PubSubDirectWriteResult{
            .consumed = consumed, .fatal = false};
      }});
  PubSubSession direct_session(4096);
  direct_session.wire_mode = WireMode::resp2;
  const std::string_view direct_channel = "fast";
  output.clear();
  direct.execute(
      direct_session,
      {.type = goblin::core::CommandType::subscribe,
       .args = std::span(&direct_channel, 1)},
      output);
  const std::string expected_push =
      "*3\r\n$7\r\nmessage\r\n$4\r\nfast\r\n$4\r\ntick\r\n";
  assert(direct.publish(direct_channel, "tick") == 1);
  assert(direct_probe.calls == 1);
  assert(direct_probe.bytes == expected_push.substr(0, direct_probe.limit));
  assert(direct_session.unsolicited.front()->sequence == 1);
  assert(direct_session.unsolicited.front()->bytes ==
         expected_push.substr(direct_probe.limit));
  direct_session.unsolicited.pop();

  direct_probe.limit = static_cast<std::size_t>(-1);
  direct_probe.bytes.clear();
  assert(direct.publish(direct_channel, "tick") == 1);
  assert(direct_probe.calls == 2);
  assert(direct_probe.bytes == expected_push);
  assert(direct_session.unsolicited.empty());
  assert(direct_session.next_output_sequence == 3);

  direct_probe.fatal = true;
  assert(direct.publish(direct_channel, "tick") == 0);
  assert(direct_session.close_requested);
  assert(direct_session.subscription_count() == 0);
#endif

#ifdef GOBLIN_HAS_SBE
  // Literal and pattern weights contribute to PUBLISH exactly as separate
  // edge clients would, while only one SBE push is queued for the whole edge.
  PubSubRegistry weighted;
  PubSubSession aggregate(4096);
  aggregate.wire_mode = WireMode::sbe;
  aggregate.bluefield_edge_id = 99;
  aggregate.bluefield_aggregate = true;
  aggregate.bluefield_aggregate_state =
      std::make_unique<goblin::core::detail::BluefieldAggregateState>();
  PubSubSession host(4096);
  host.wire_mode = WireMode::resp2;
  const std::string_view weighted_channel = "prices:weighted";
  const std::string_view weighted_pattern = "prices:*";
  output.clear();
  weighted.execute(
      aggregate,
      {.type = goblin::core::CommandType::subscribe,
       .args = std::span(&weighted_channel, 1)},
      output);
  weighted.execute(
      aggregate,
      {.type = goblin::core::CommandType::psubscribe,
       .args = std::span(&weighted_pattern, 1)},
      output);
  weighted.execute(
      host,
      {.type = goblin::core::CommandType::subscribe,
       .args = std::span(&weighted_channel, 1)},
      output);
  assert(weighted.set_bluefield_subscription_weight(
      aggregate, weighted_channel, false, 2));
  assert(weighted.set_bluefield_subscription_weight(
      aggregate, weighted_pattern, true, 3));
  aggregate.unsolicited.clear();  // discard subscription acknowledgements
  host.unsolicited.clear();
  assert(weighted.publish(weighted_channel, "tick") == 6);
  assert(aggregate.unsolicited.front().has_value());
  aggregate.unsolicited.pop();
  assert(aggregate.unsolicited.empty());
  host.unsolicited.clear();

  // Publications originating from this edge skip its aggregate subscription.
  assert(weighted.publish(weighted_channel, "self", 99) == 1);
  assert(aggregate.unsolicited.empty());

  PubSubSession query(4096);
  query.wire_mode = WireMode::resp2;
  const std::string_view numsub_args[]{"NUMSUB", weighted_channel};
  output.clear();
  weighted.execute(
      query,
      {.type = goblin::core::CommandType::pubsub, .args = numsub_args}, output);
  assert(output == "*2\r\n$15\r\nprices:weighted\r\n:3\r\n");
#endif
}
