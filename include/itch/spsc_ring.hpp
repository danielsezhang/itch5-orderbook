#pragma once

#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>

namespace itch {

// Bounded wait-free queue for exactly one producer thread and one consumer thread.
//
// head_ and tail_ are free-running counters on separate cache lines; each side also keeps a
// private copy of the other side's counter and reloads it only when the copy says the ring is
// full (producer) or empty (consumer). In steady state the two cores therefore exchange a cache
// line once per batch rather than once per element.
template <typename T, std::size_t Capacity>
class SpscRing {
  static_assert(std::has_single_bit(Capacity), "capacity must be a power of two");
  static_assert(std::is_trivially_copyable_v<T>);

 public:
  // Producer only. Returns false when the ring is full.
  [[nodiscard]] bool try_push(const T& value) noexcept {
    const std::uint64_t tail = tail_.load(std::memory_order_relaxed);
    if (tail - producer_.cached_head == Capacity) {
      producer_.cached_head = head_.load(std::memory_order_acquire);
      if (tail - producer_.cached_head == Capacity) return false;
    }
    slots_[tail & kMask] = value;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Consumer only. Passes every element published so far to `consume`, oldest first, then
  // frees their slots with a single store. Returns how many elements were consumed.
  template <typename Consume>
  std::size_t drain(Consume&& consume) {
    std::uint64_t head = head_.load(std::memory_order_relaxed);
    if (head == consumer_.cached_tail) {
      consumer_.cached_tail = tail_.load(std::memory_order_acquire);
      if (head == consumer_.cached_tail) return 0;
    }
    const std::uint64_t end = consumer_.cached_tail;
    const auto count = static_cast<std::size_t>(end - head);
    for (; head != end; ++head) consume(slots_[head & kMask]);
    head_.store(end, std::memory_order_release);
    return count;
  }

 private:
  static constexpr std::uint64_t kMask = Capacity - 1;
  static constexpr std::size_t kLine = 64;

  struct alignas(kLine) ProducerState {
    std::uint64_t cached_head{};
  };
  struct alignas(kLine) ConsumerState {
    std::uint64_t cached_tail{};
  };

  alignas(kLine) std::atomic<std::uint64_t> head_{0};
  alignas(kLine) std::atomic<std::uint64_t> tail_{0};
  ProducerState producer_;
  ConsumerState consumer_;
  alignas(kLine) std::array<T, Capacity> slots_{};
};

}  // namespace itch
