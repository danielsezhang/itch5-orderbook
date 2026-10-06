#include "itch/market_replay.hpp"
#include "itch/parser.hpp"
#include "itch/sharded_replay.hpp"
#include "itch/spsc_ring.hpp"
#include "book_compare.hpp"
#include "itch_stream.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <new>
#include <random>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using itch::testing::diff_books;
using itch::testing::ItchStream;

// A random multi-symbol session: adds, partial and full executions, cancels, deletes, replaces,
// plus a few references the book has never seen.
ItchStream random_session(unsigned symbols, std::size_t messages, std::uint64_t seed) {
  struct Live {
    std::uint16_t locate;
    std::uint64_t reference;
    std::uint32_t shares;
  };
  std::mt19937_64 rng(seed);
  std::vector<Live> live;
  std::uint64_t next_reference = 1;
  ItchStream stream;
  const auto price = [&] { return 1'000'000 + static_cast<std::uint32_t>(rng() % 400) * 100; };

  for (std::size_t i = 0; i < messages; ++i) {
    const auto roll = rng() % 100;
    if (live.empty() || roll < 45) {
      const auto locate = static_cast<std::uint16_t>(1 + rng() % symbols);
      const auto shares = static_cast<std::uint32_t>(1 + rng() % 500);
      stream.add(locate, next_reference, rng() % 2 ? 'B' : 'S', shares, price());
      live.push_back({locate, next_reference++, shares});
      continue;
    }
    if (roll < 47) {
      stream.remove(static_cast<std::uint16_t>(1 + rng() % symbols), 1'000'000'000 + i);
      continue;
    }
    const std::size_t index = rng() % live.size();
    Live& order = live[index];
    bool gone = false;
    if (roll < 80) {
      const auto shares = static_cast<std::uint32_t>(1 + rng() % order.shares);
      if (roll < 65)
        stream.execute(order.locate, order.reference, shares);
      else
        stream.cancel(order.locate, order.reference, shares);
      order.shares -= shares;
      gone = order.shares == 0;
    } else if (roll < 90) {
      stream.remove(order.locate, order.reference);
      gone = true;
    } else {
      const auto shares = static_cast<std::uint32_t>(1 + rng() % 500);
      stream.replace(order.locate, order.reference, next_reference, shares, price());
      order = {order.locate, next_reference++, shares};
    }
    if (gone) {
      live[index] = live.back();
      live.pop_back();
    }
  }
  return stream;
}

}  // namespace

TEST_CASE("SPSC ring delivers every element exactly once, in order, across threads") {
  constexpr std::uint64_t kItems = 2'000'000;
  itch::SpscRing<std::uint64_t, 64> ring;  // tiny, so the full and empty paths run constantly
  std::thread producer([&] {
    for (std::uint64_t i = 0; i < kItems; ++i) {
      while (!ring.try_push(i)) std::this_thread::yield();
    }
  });
  std::uint64_t expected = 0;
  bool in_order = true;
  while (expected < kItems) {
    ring.drain([&](std::uint64_t value) { in_order &= value == expected++; });
  }
  producer.join();
  REQUIRE(in_order);
  REQUIRE(ring.drain([](std::uint64_t) {}) == 0);
}

TEST_CASE("sharded replay builds exactly the books the sequential replay builds") {
  const unsigned shards = GENERATE(1u, 2u, 3u, 8u);
  const ItchStream stream = random_session(/*symbols=*/50, /*messages=*/300'000, /*seed=*/shards);

  itch::OrderPool pool(std::size_t{1} << 20);
  itch::MarketReplay sequential(pool);
  itch::Parser(stream.data(), stream.size()).parse(sequential);

  itch::ShardedReplay sharded(pool, shards);
  REQUIRE(sharded.replay(stream.data(), stream.size()) == stream.messages());

  REQUIRE(sharded.stats() == sequential.stats());
  REQUIRE(sharded.stats().anomalies() > 0);  // the injected unknown references were counted
  REQUIRE(sharded.book_count() == sequential.book_count());
  REQUIRE(sharded.open_orders() == sequential.open_orders());
  std::uint64_t routed = 0;
  for (unsigned shard = 0; shard < shards; ++shard) routed += sharded.routed(shard);
  REQUIRE(routed == sequential.stats().order_messages());

  for (std::uint32_t locate = 0; locate <= UINT16_MAX; ++locate) {
    const auto* expected = sequential.book(static_cast<std::uint16_t>(locate));
    const auto* actual = sharded.book(static_cast<std::uint16_t>(locate));
    REQUIRE((expected == nullptr) == (actual == nullptr));
    if (expected != nullptr) {
      INFO("locate " << locate);
      REQUIRE(diff_books(*expected, *actual).empty());
    }
  }
}

TEST_CASE("sharded replay stops every thread and rethrows a parse error") {
  const ItchStream stream = random_session(10, 10'000, 7);
  itch::OrderPool pool(std::size_t{1} << 16);
  itch::ShardedReplay sharded(pool, 4);
  REQUIRE_THROWS_AS(sharded.replay(stream.data(), stream.size() - 1), std::runtime_error);
}

TEST_CASE("sharded replay rethrows an error raised on a shard thread") {
  ItchStream stream;
  for (std::uint64_t reference = 1; reference <= 1'000; ++reference) {
    stream.add(1, reference, 'B', 100, 1'000'000);
  }
  itch::OrderPool pool(itch::OrderPool::kBatch * 4);  // too small for 1,000 resting orders
  itch::ShardedReplay sharded(pool, 2);
  REQUIRE_THROWS_AS(sharded.replay(stream.data(), stream.size()), std::bad_alloc);
}
