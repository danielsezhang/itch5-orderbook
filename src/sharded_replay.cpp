#include "itch/sharded_replay.hpp"

#include "itch/parser.hpp"
#include "itch/spsc_ring.hpp"

#include <atomic>
#include <exception>
#include <stdexcept>
#include <thread>

namespace itch {
namespace {

// 16,384 pointers (128 KiB) per shard absorbs bursts without letting one slow shard fall far
// behind the parser.
constexpr std::size_t kRingCapacity = std::size_t{1} << 14;

void cpu_relax() noexcept {
#if defined(__aarch64__)
  asm volatile("yield");
#elif defined(__x86_64__)
  __builtin_ia32_pause();
#endif
}

// Spin briefly, then give the core away: keeps hand-off latency low without starving the other
// side when there are more runnable threads than cores.
class Backoff {
 public:
  void pause() noexcept {
    if (spins_ < kSpinLimit) {
      ++spins_;
      cpu_relax();
    } else {
      std::this_thread::yield();
    }
  }
  void reset() noexcept { spins_ = 0; }

 private:
  static constexpr int kSpinLimit = 64;
  int spins_{0};
};

}  // namespace

struct ShardedReplay::Shard {
  Shard(OrderPool& pool, std::uint32_t window_levels) : books(pool, window_levels) {}

  // Runs on the shard thread until the producer has closed the stream and the ring is empty.
  void run(const std::atomic<bool>& closed) noexcept {
    try {
      consume(closed, [this](const std::byte* message) { dispatch_order_message(message, books); });
    } catch (...) {
      error = std::current_exception();
      // Keep draining so the producer never blocks on a ring nobody empties.
      consume(closed, [](const std::byte*) {});
    }
  }

  template <typename Apply>
  void consume(const std::atomic<bool>& closed, Apply apply) {
    Backoff idle;
    for (;;) {
      if (ring.drain(apply) != 0) {
        idle.reset();
        continue;
      }
      // The producer publishes every message before it sets `closed`, so one more drain after
      // observing it is guaranteed to see the tail of the stream.
      if (closed.load(std::memory_order_acquire)) {
        ring.drain(apply);
        return;
      }
      idle.pause();
    }
  }

  SpscRing<const std::byte*, kRingCapacity> ring;
  MarketReplay books;
  std::exception_ptr error;
};

ShardedReplay::ShardedReplay(OrderPool& pool, unsigned shards, std::uint32_t window_levels)
    : routed_(shards) {
  if (shards == 0) throw std::invalid_argument("ShardedReplay needs at least one shard");
  shards_.reserve(shards);
  for (unsigned i = 0; i < shards; ++i) {
    shards_.push_back(std::make_unique<Shard>(pool, window_levels));
  }
}

ShardedReplay::~ShardedReplay() = default;

std::uint64_t ShardedReplay::replay(const std::byte* data, std::size_t size) {
  std::atomic<bool> closed{false};
  std::uint64_t messages = 0;
  {
    std::vector<std::thread> workers;
    // Closes the stream and joins every started worker on scope exit, including when parsing
    // throws, so no shard thread outlives this call.
    struct CloseAndJoin {
      std::atomic<bool>& closed;
      std::vector<std::thread>& workers;
      ~CloseAndJoin() {
        closed.store(true, std::memory_order_release);
        for (auto& worker : workers) worker.join();
      }
    } close_and_join{closed, workers};

    workers.reserve(shards_.size());
    for (auto& shard : shards_) {
      workers.emplace_back([&shard, &closed] { shard->run(closed); });
    }
    messages = Parser(data, size).for_each_message([this](const std::byte* message) {
      if (is_order_message(static_cast<char>(*message))) route(message);
    });
  }
  for (const auto& shard : shards_) {
    if (shard->error) std::rethrow_exception(shard->error);
  }
  return messages;
}

void ShardedReplay::route(const std::byte* message) noexcept {
  const auto& header = *reinterpret_cast<const MessageHeader*>(message);
  const unsigned shard = shard_of(header.locate());
  ++routed_[shard];
  Backoff full;
  while (!shards_[shard]->ring.try_push(message)) full.pause();
}

const OrderBook* ShardedReplay::book(std::uint16_t locate) const noexcept {
  return shards_[shard_of(locate)]->books.book(locate);
}

ReplayStats ShardedReplay::stats() const noexcept {
  ReplayStats total{};
  for (const auto& shard : shards_) total += shard->books.stats();
  return total;
}

std::size_t ShardedReplay::book_count() const noexcept {
  std::size_t total = 0;
  for (const auto& shard : shards_) total += shard->books.book_count();
  return total;
}

std::size_t ShardedReplay::open_orders() const noexcept {
  std::size_t total = 0;
  for (const auto& shard : shards_) total += shard->books.open_orders();
  return total;
}

}  // namespace itch
