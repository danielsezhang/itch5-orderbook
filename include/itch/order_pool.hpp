#pragma once

#include "itch/order.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace itch {

// Fixed-capacity slab allocator for Order nodes.
//
// allocate()/release() touch only a thread-local cache. Whole batches move between threads
// through a lock-free Treiber stack whose head packs {batch pointer, 64-bit tag} into one
// 128-bit word: every push and pop bumps the tag, so a head that is popped, reused, and pushed
// back between another thread's load and CAS no longer compares equal (ABA).
//
// Lifetime contract: a thread that used a pool must exit, or call flush_thread_cache(), before
// that pool is destroyed. The destructor flushes the calling thread's cache itself.
class OrderPool {
 public:
  static constexpr std::uint32_t kBatch = 64;

  explicit OrderPool(std::size_t capacity = std::size_t{1} << 23);
  ~OrderPool();
  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;

  // Returns uninitialised storage for one Order; throws std::bad_alloc when exhausted.
  [[nodiscard]] Order* allocate();
  void release(Order* order) noexcept;

  // Returns the calling thread's cached slots to the shared stack.
  static void flush_thread_cache() noexcept;

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
  [[nodiscard]] std::size_t index_of_slot(const Order* order) const noexcept {
    return index_of(order);
  }
  [[nodiscard]] static bool cas128_is_lock_free() noexcept {
    return std::atomic<unsigned __int128>::is_always_lock_free;
  }

 private:
  struct ThreadCache {
    OrderPool* owner{};
    std::uint32_t count{};
    std::uint32_t slots[2 * kBatch]{};
    ~ThreadCache();
  };
  static thread_local ThreadCache cache_;

  void adopt(ThreadCache& cache) noexcept;
  void refill(ThreadCache& cache);
  void push_batch(const std::uint32_t* slots, std::uint32_t count) noexcept;
  void drain(ThreadCache& cache) noexcept;
  [[nodiscard]] Order* slot(std::uint32_t index) const noexcept { return slab_ + index; }
  [[nodiscard]] std::uint32_t index_of(const Order* order) const noexcept {
    return static_cast<std::uint32_t>(order - slab_);
  }

  std::size_t capacity_;
  std::size_t slab_bytes_;
  std::size_t link_bytes_;
  Order* slab_{};
  // Links store index + 1 so zero means null. batch_next_ is read by competing poppers, so it
  // is only accessed atomically; chain_next_ is private to whichever thread owns the batch.
  std::uint32_t* batch_next_{};
  std::uint32_t* chain_next_{};
  std::atomic<std::uint64_t> carved_{0};
  alignas(64) std::atomic<unsigned __int128> head_{0};
};

}  // namespace itch
