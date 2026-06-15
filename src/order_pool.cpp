#include "itch/order_pool.hpp"

#include <new>
#include <stdexcept>

#include <sys/mman.h>

namespace itch {
namespace {
constexpr std::size_t kBatchSize = 64;

struct LocalCache {
  OrderPool* pool{};
  std::vector<Order*> orders;
};

thread_local LocalCache local_cache;

#if defined(__x86_64__)
unsigned __int128 pack(Order* pointer, std::uint64_t tag) noexcept {
  return (static_cast<unsigned __int128>(tag) << 64) |
         reinterpret_cast<std::uintptr_t>(pointer);
}

Order* unpack_pointer(unsigned __int128 value) noexcept {
  return reinterpret_cast<Order*>(static_cast<std::uintptr_t>(value));
}

std::uint64_t unpack_tag(unsigned __int128 value) noexcept {
  return static_cast<std::uint64_t>(value >> 64);
}
#endif
}  // namespace

OrderPool::OrderPool(std::size_t capacity) : bytes_(capacity * sizeof(Order)) {
  int flags = MAP_PRIVATE | MAP_ANON;
#ifdef MAP_HUGETLB
  memory_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, flags | MAP_HUGETLB, -1, 0);
  if (memory_ == MAP_FAILED) memory_ = nullptr;
#endif
  if (memory_ == nullptr) {
    memory_ = mmap(nullptr, bytes_, PROT_READ | PROT_WRITE, flags, -1, 0);
  }
  if (memory_ == MAP_FAILED) throw std::bad_alloc();

  auto* slots = static_cast<Order*>(memory_);
  for (std::size_t index = 0; index < capacity; ++index) push_central(slots + index);
}

OrderPool::~OrderPool() {
  if (memory_ != nullptr && memory_ != MAP_FAILED) munmap(memory_, bytes_);
}

Order* OrderPool::create(std::uint64_t reference, Side side, std::uint32_t shares, std::uint32_t price) {
  auto* slot = static_cast<Order*>(allocate_slot());
  return ::new (static_cast<void*>(slot)) Order{reference, shares, price, side, nullptr, nullptr};
}

void* OrderPool::allocate_slot() {
  if (local_cache.pool != this) {
    local_cache.orders.clear();
    local_cache.pool = this;
  }
  if (local_cache.orders.empty()) refill_local();
  auto* slot = local_cache.orders.back();
  local_cache.orders.pop_back();
  return slot;
}

void OrderPool::destroy(Order* order) noexcept {
  order->~Order();
  release_slot(order);
}

void OrderPool::release_slot(Order* order) noexcept {
  if (local_cache.pool != this) {
    push_central(order);
    return;
  }
  local_cache.orders.push_back(order);
  if (local_cache.orders.size() > kBatchSize * 2) {
    for (std::size_t index = 0; index < kBatchSize; ++index) {
      push_central(local_cache.orders.back());
      local_cache.orders.pop_back();
    }
  }
}

void* Order::operator new(std::size_t size) {
  if (size != sizeof(Order)) throw std::bad_alloc();
  return OrderPool::default_instance().allocate_slot();
}

void Order::operator delete(void* pointer) noexcept {
  OrderPool::default_instance().release_slot(static_cast<Order*>(pointer));
}

OrderPool& OrderPool::default_instance() {
  static OrderPool pool;
  return pool;
}

void OrderPool::refill_local() {
  for (std::size_t index = 0; index < kBatchSize; ++index) {
    if (auto* order = pop_central()) local_cache.orders.push_back(order);
    else break;
  }
  if (local_cache.orders.empty()) throw std::bad_alloc();
}

Order* OrderPool::pop_central() noexcept {
#if defined(__x86_64__)
  auto observed = head_.load(std::memory_order_acquire);
  while (auto* pointer = unpack_pointer(observed)) {
    const auto desired = pack(pointer->next, unpack_tag(observed) + 1);
    if (head_.compare_exchange_weak(observed, desired, std::memory_order_acq_rel,
                                    std::memory_order_acquire)) return pointer;
  }
  return nullptr;
#else
  std::lock_guard lock(mutex_);
  if (free_.empty()) return nullptr;
  auto* order = free_.back();
  free_.pop_back();
  return order;
#endif
}

void OrderPool::push_central(Order* order) noexcept {
#if defined(__x86_64__)
  auto observed = head_.load(std::memory_order_acquire);
  do {
    order->next = unpack_pointer(observed);
  } while (!head_.compare_exchange_weak(observed, pack(order, unpack_tag(observed) + 1),
                                        std::memory_order_release, std::memory_order_acquire));
#else
  std::lock_guard lock(mutex_);
  free_.push_back(order);
#endif
}

}  // namespace itch
