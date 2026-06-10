#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace itch {

class OrderPool;

enum class Side : char { bid = 'B', ask = 'S' };

struct Order {
  std::uint64_t reference{};
  std::uint32_t shares{};
  std::uint32_t price{};
  Side side{};
  Order* prev{};
  Order* next{};

  static void* operator new(std::size_t size);
  static void operator delete(void* pointer) noexcept;
};

struct PriceLevel {
  Order* head{};
  Order* tail{};
  std::uint64_t displayed_shares{};

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};

struct BestQuote {
  std::uint32_t price{};
  std::uint64_t shares{};
  [[nodiscard]] bool valid() const noexcept { return shares != 0; }
};

class OrderBook {
 public:
  explicit OrderBook(std::size_t max_price_ticks = 5'000'000);
  ~OrderBook();
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  void add(std::uint64_t reference, Side side, std::uint32_t shares, std::uint32_t price);
  void execute(std::uint64_t reference, std::uint32_t shares);
  void cancel(std::uint64_t reference, std::uint32_t shares);
  void replace(std::uint64_t original_reference, std::uint64_t replacement_reference,
               std::uint32_t shares, std::uint32_t price);

  [[nodiscard]] BestQuote best_bid() const noexcept;
  [[nodiscard]] BestQuote best_ask() const noexcept;
  [[nodiscard]] std::size_t order_count() const noexcept;

 private:
  class ReferenceMap;
  void remove(Order& order);
  void reduce(Order& order, std::uint32_t shares);
  [[nodiscard]] PriceLevel& level_for(const Order& order) noexcept;
  [[nodiscard]] const PriceLevel& level_for(const Order& order) const noexcept;
  void refresh_best(Side side);

  std::unique_ptr<OrderPool> pool_;
  std::unique_ptr<ReferenceMap> references_;
  std::vector<PriceLevel> bids_;
  std::vector<PriceLevel> asks_;
  BestQuote best_bid_{};
  BestQuote best_ask_{};
};

}  // namespace itch
