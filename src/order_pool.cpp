#include "itch/order_pool.hpp"

#include <algorithm>
#include <new>

#include <sys/mman.h>

namespace itch {
namespace {

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

void* map_zeroed(std::size_t bytes) {
  void* memory =
      mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
  if (memory == MAP_FAILED) throw std::bad_alloc();
  return memory;
}

unsigned __int128 pack(const Order* pointer, std::uint64_t tag) noexcept {
  return (static_cast<unsigned __int128>(tag) << 64) | reinterpret_cast<std::uintptr_t>(pointer);
}

Order* pointer_of(unsigned __int128 head) noexcept {
  return reinterpret_cast<Order*>(static_cast<std::uintptr_t>(head));
}

std::uint64_t tag_of(unsigned __int128 head) noexcept {
  return static_cast<std::uint64_t>(head >> 64);
}

}  // namespace

thread_local OrderPool::ThreadCache OrderPool::cache_;

OrderPool::ThreadCache::~ThreadCache() {
  if (owner != nullptr) owner->drain(*this);
}

OrderPool::OrderPool(std::size_t capacity)
    : capacity_(capacity),
      slab_bytes_(capacity * sizeof(Order)),
      link_bytes_(capacity * sizeof(std::uint32_t)) {
  if (capacity == 0 || capacity >= UINT32_MAX) throw std::bad_alloc();
  slab_ = static_cast<Order*>(map_zeroed(slab_bytes_));
  batch_next_ = static_cast<std::uint32_t*>(map_zeroed(link_bytes_));
  chain_next_ = static_cast<std::uint32_t*>(map_zeroed(link_bytes_));
}

OrderPool::~OrderPool() {
  if (cache_.owner == this) {
    cache_.owner = nullptr;
    cache_.count = 0;
  }
  munmap(slab_, slab_bytes_);
  munmap(batch_next_, link_bytes_);
  munmap(chain_next_, link_bytes_);
}

Order* OrderPool::allocate() {
  ThreadCache& cache = cache_;
  if (cache.owner != this) [[unlikely]]
    adopt(cache);
  if (cache.count == 0) [[unlikely]]
    refill(cache);
  return slot(cache.slots[--cache.count]);
}

void OrderPool::release(Order* order) noexcept {
  ThreadCache& cache = cache_;
  if (cache.owner != this) [[unlikely]]
    adopt(cache);
  cache.slots[cache.count++] = index_of(order);
  if (cache.count == 2 * kBatch) [[unlikely]] {
    push_batch(cache.slots + kBatch, kBatch);
    cache.count = kBatch;
  }
}

void OrderPool::flush_thread_cache() noexcept {
  if (cache_.owner != nullptr) cache_.owner->drain(cache_);
}

void OrderPool::adopt(ThreadCache& cache) noexcept {
  if (cache.owner != nullptr) cache.owner->drain(cache);
  cache.owner = this;
  cache.count = 0;
}

void OrderPool::drain(ThreadCache& cache) noexcept {
  for (std::uint32_t begin = 0; begin < cache.count; begin += kBatch) {
    push_batch(cache.slots + begin, std::min(kBatch, cache.count - begin));
  }
  cache.count = 0;
  cache.owner = nullptr;
}

void OrderPool::push_batch(const std::uint32_t* slots, std::uint32_t count) noexcept {
  for (std::uint32_t i = 0; i + 1 < count; ++i) chain_next_[slots[i]] = slots[i + 1] + 1;
  chain_next_[slots[count - 1]] = 0;

  const std::uint32_t first = slots[0];
  auto observed = head_.load(std::memory_order_relaxed);
  for (;;) {
    const Order* top = pointer_of(observed);
    const std::uint32_t link = top == nullptr ? 0 : index_of(top) + 1;
    __atomic_store_n(&batch_next_[first], link, __ATOMIC_RELAXED);
    if (head_.compare_exchange_weak(observed, pack(slot(first), tag_of(observed) + 1),
                                    std::memory_order_release, std::memory_order_relaxed)) {
      return;
    }
  }
}

void OrderPool::refill(ThreadCache& cache) {
  auto observed = head_.load(std::memory_order_acquire);
  while (Order* top = pointer_of(observed)) {
    // A stale `link` is harmless: if `top` was popped and pushed back meanwhile, its tag moved
    // on and this CAS fails.
    const std::uint32_t link = __atomic_load_n(&batch_next_[index_of(top)], __ATOMIC_RELAXED);
    const Order* next = link == 0 ? nullptr : slot(link - 1);
    if (head_.compare_exchange_weak(observed, pack(next, tag_of(observed) + 1),
                                    std::memory_order_acquire, std::memory_order_acquire)) {
      for (std::uint32_t index = index_of(top) + 1; index != 0; index = chain_next_[index - 1]) {
        cache.slots[cache.count++] = index - 1;
      }
      return;
    }
  }

  const std::uint64_t start = carved_.fetch_add(kBatch, std::memory_order_relaxed);
  if (start >= capacity_) throw std::bad_alloc();
  const std::uint64_t end = std::min<std::uint64_t>(start + kBatch, capacity_);
  // Hand out ascending addresses first so a fresh book touches the slab sequentially.
  for (std::uint64_t index = end; index-- > start;) {
    cache.slots[cache.count++] = static_cast<std::uint32_t>(index);
  }
}

}  // namespace itch
