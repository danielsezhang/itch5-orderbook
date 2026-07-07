#pragma once

#include "itch/market_replay.hpp"
#include "itch/order_book.hpp"
#include "itch/order_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace itch {

// Multi-threaded Level 3 reconstruction.
//
//   calling thread:  parse ──► route by stock locate ──► SPSC ring per shard
//   shard thread i:  drain ring i ──► MarketReplay i (the books whose locate % shards == i)
//
// The rings carry pointers into the mapped feed, so a message is never copied. Each symbol is
// owned by exactly one shard and its messages travel through one FIFO ring, so per-symbol
// message order is preserved and no book is ever shared between threads. Every shard allocates
// orders from the same OrderPool through its own thread-local cache.
class ShardedReplay {
 public:
  ShardedReplay(OrderPool& pool, unsigned shards,
                std::uint32_t window_levels = OrderBook::kDefaultWindowLevels);
  ~ShardedReplay();
  ShardedReplay(const ShardedReplay&) = delete;
  ShardedReplay& operator=(const ShardedReplay&) = delete;

  // Replays a BinaryFILE stream and blocks until every order message has been applied. Returns
  // the number of messages parsed. Rethrows the first error raised on any thread, after all
  // shard threads have stopped. Call at most once per instance.
  std::uint64_t replay(const std::byte* data, std::size_t size);

  [[nodiscard]] unsigned shard_count() const noexcept {
    return static_cast<unsigned>(shards_.size());
  }
  [[nodiscard]] unsigned shard_of(std::uint16_t locate) const noexcept {
    return locate % shard_count();
  }
  [[nodiscard]] const OrderBook* book(std::uint16_t locate) const noexcept;
  // Order messages routed to one shard; shows how evenly symbols spread the load.
  [[nodiscard]] std::uint64_t routed(unsigned shard) const noexcept { return routed_[shard]; }
  [[nodiscard]] ReplayStats stats() const noexcept;
  [[nodiscard]] std::size_t book_count() const noexcept;
  [[nodiscard]] std::size_t open_orders() const noexcept;

 private:
  struct Shard;

  void route(const std::byte* message) noexcept;

  std::vector<std::unique_ptr<Shard>> shards_;
  std::vector<std::uint64_t> routed_;
};

}  // namespace itch
