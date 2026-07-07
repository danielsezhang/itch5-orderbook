#pragma once

#include "itch/messages.hpp"
#include "itch/order_book.hpp"
#include "itch/order_pool.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace itch {

struct ReplayStats {
  std::uint64_t adds{};
  std::uint64_t executes{};
  std::uint64_t executes_with_price{};
  std::uint64_t cancels{};
  std::uint64_t deletes{};
  std::uint64_t replaces{};
  std::uint64_t unknown_reference{};
  std::uint64_t duplicate_reference{};
  std::uint64_t exceeds_open_shares{};
  std::uint64_t invalid{};

  [[nodiscard]] std::uint64_t order_messages() const noexcept {
    return adds + executes + executes_with_price + cancels + deletes + replaces;
  }
  // Messages the book could not apply cleanly; zero on a correct reconstruction.
  [[nodiscard]] std::uint64_t anomalies() const noexcept {
    return unknown_reference + duplicate_reference + exceeds_open_shares + invalid;
  }
  ReplayStats& operator+=(const ReplayStats& other) noexcept;
  bool operator==(const ReplayStats&) const = default;
};

// Parser visitor that maintains one Level 3 book per stock locate code, created on first use.
class MarketReplay {
 public:
  explicit MarketReplay(OrderPool& pool,
                        std::uint32_t window_levels = OrderBook::kDefaultWindowLevels);

  void on(const AddOrder& message);
  void on(const ExecuteOrder& message);
  void on(const ExecuteWithPrice& message);
  void on(const CancelOrder& message);
  void on(const DeleteOrder& message);
  void on(const ReplaceOrder& message);

  [[nodiscard]] const OrderBook* book(std::uint16_t locate) const noexcept {
    return books_[locate].get();
  }
  [[nodiscard]] const ReplayStats& stats() const noexcept { return stats_; }
  [[nodiscard]] std::size_t book_count() const noexcept;
  [[nodiscard]] std::size_t open_orders() const noexcept;

 private:
  OrderBook& book_for(std::uint16_t locate);
  void record(Status status) noexcept;

  OrderPool& pool_;
  std::uint32_t window_levels_;
  std::vector<std::unique_ptr<OrderBook>> books_;
  ReplayStats stats_{};
};

}  // namespace itch
