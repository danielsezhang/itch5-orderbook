#include "itch/order_pool.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <new>
#include <random>
#include <set>
#include <thread>
#include <vector>

TEST_CASE("128-bit CAS is lock-free on this target") {
  REQUIRE(itch::OrderPool::cas128_is_lock_free());
}

TEST_CASE("pool hands out unique slots, recycles them, and reports exhaustion") {
  constexpr std::size_t capacity = 4 * itch::OrderPool::kBatch;
  itch::OrderPool pool(capacity);
  std::vector<itch::Order*> slots;
  std::set<itch::Order*> unique;
  for (std::size_t i = 0; i < capacity; ++i) {
    slots.push_back(pool.allocate());
    unique.insert(slots.back());
    REQUIRE(pool.index_of_slot(slots.back()) < capacity);
  }
  REQUIRE(unique.size() == capacity);
  REQUIRE_THROWS_AS(pool.allocate(), std::bad_alloc);

  for (auto* slot : slots) pool.release(slot);
  itch::OrderPool::flush_thread_cache();
  std::set<itch::Order*> again;
  for (std::size_t i = 0; i < capacity; ++i) again.insert(pool.allocate());
  REQUIRE(again == unique);
}

TEST_CASE("slots released on one thread are reused on another through the shared stack") {
  itch::OrderPool pool(8 * itch::OrderPool::kBatch);
  std::vector<itch::Order*> slots;
  std::thread producer([&] {
    for (int i = 0; i < 3 * static_cast<int>(itch::OrderPool::kBatch); ++i) slots.push_back(pool.allocate());
    // Thread exit drains nothing here: every slot is still live.
  });
  producer.join();
  for (auto* slot : slots) pool.release(slot);  // main thread caches, then spills batches
  itch::OrderPool::flush_thread_cache();
  std::set<itch::Order*> reused;
  std::thread consumer([&] {
    for (std::size_t i = 0; i < slots.size(); ++i) reused.insert(pool.allocate());
    for (auto* slot : reused) pool.release(slot);
  });
  consumer.join();
  REQUIRE(reused == std::set<itch::Order*>(slots.begin(), slots.end()));
}

TEST_CASE("concurrent allocate and cross-thread release never double-allocates or leaks") {
  constexpr int kThreads = 8;
  constexpr int kRounds = 40'000;
  // Small pool: heads of the shared stack are recycled constantly, which is when an untagged
  // Treiber stack would suffer ABA and hand the same slot to two threads.
  constexpr std::size_t capacity = kThreads * 6 * itch::OrderPool::kBatch;
  itch::OrderPool pool(capacity);
  auto in_use = std::make_unique<std::atomic<bool>[]>(capacity);
  std::atomic<std::uint64_t> double_allocations{0};

  std::mutex handoff_mutex;
  std::vector<itch::Order*> handoff;

  std::vector<std::thread> threads;
  for (int t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t));
      std::vector<itch::Order*> mine;
      for (int round = 0; round < kRounds; ++round) {
        const int burst = 1 + static_cast<int>(rng() % 96);
        for (int i = 0; i < burst; ++i) {
          itch::Order* slot = pool.allocate();
          if (in_use[pool.index_of_slot(slot)].exchange(true)) ++double_allocations;
          slot->reference = static_cast<std::uint64_t>(t);
          mine.push_back(slot);
        }
        // Give half away so another thread releases memory this thread allocated.
        {
          std::lock_guard lock(handoff_mutex);
          for (std::size_t i = 0; i < mine.size() / 2; ++i) handoff.push_back(mine[i]);
          mine.erase(mine.begin(), mine.begin() + static_cast<std::ptrdiff_t>(mine.size() / 2));
          const std::size_t take = std::min<std::size_t>(handoff.size(), 64);
          mine.insert(mine.end(), handoff.end() - static_cast<std::ptrdiff_t>(take), handoff.end());
          handoff.resize(handoff.size() - take);
        }
        for (auto* slot : mine) {
          in_use[pool.index_of_slot(slot)].store(false);
          pool.release(slot);
        }
        mine.clear();
      }
      itch::OrderPool::flush_thread_cache();
    });
  }
  for (auto& thread : threads) thread.join();
  for (auto* slot : handoff) {
    in_use[pool.index_of_slot(slot)].store(false);
    pool.release(slot);
  }
  itch::OrderPool::flush_thread_cache();

  REQUIRE(double_allocations.load() == 0);
  // Conservation: every slot is back, so the full capacity can be allocated once more.
  std::set<itch::Order*> all;
  for (std::size_t i = 0; i < capacity; ++i) all.insert(pool.allocate());
  REQUIRE(all.size() == capacity);
  REQUIRE_THROWS_AS(pool.allocate(), std::bad_alloc);
}
