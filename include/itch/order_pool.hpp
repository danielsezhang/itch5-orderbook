#pragma once

#include "itch/order_book.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

#if defined(__x86_64__)
#include <atomic>
#endif

namespace itch {

class OrderPool {
 public:
  explicit OrderPool(std::size_t capacity = 65'536);
  ~OrderPool();
  OrderPool(const OrderPool&) = delete;
  OrderPool& operator=(const OrderPool&) = delete;

  Order* create(std::uint64_t reference, Side side, std::uint32_t shares, std::uint32_t price);
  void destroy(Order* order) noexcept;
  void* allocate_slot();
  void release_slot(Order* order) noexcept;
  static OrderPool& default_instance();

 private:
  Order* pop_central() noexcept;
  void push_central(Order* order) noexcept;
  void refill_local();

  void* memory_{};
  std::size_t bytes_{};
#if defined(__x86_64__)
  std::atomic<unsigned __int128> head_{};
#else
  std::mutex mutex_;
  std::vector<Order*> free_;
#endif
};

}  // namespace itch
