#pragma once

#include "itch/order.hpp"
#include "itch/order_pool.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

namespace itch {

struct PriceLevel {
  Order* head{};
  Order* tail{};
  std::uint64_t shares{};

  [[nodiscard]] bool empty() const noexcept { return head == nullptr; }
};

struct BestQuote {
  std::uint32_t price{};
  std::uint64_t shares{};
  [[nodiscard]] bool valid() const noexcept { return shares != 0; }
};

struct LevelView {
  std::uint32_t price;
  const PriceLevel* level;
};

enum class Status { ok, unknown_reference, duplicate_reference, exceeds_open_shares, invalid };

struct SideStats {
  std::uint64_t window_inserts{};
  std::uint64_t overflow_inserts{};
  std::uint64_t recenters{};
};

namespace detail {

// One side of the book. On-grid prices inside a sliding window live in a flat array indexed
// by price, so locating a level is arithmetic. A two-level bitmap over the window finds the
// best level with two count-leading/trailing-zero instructions. Prices outside the window (or
// off the Reg NMS tick grid) fall back to a sorted map, which stays small because the window
// re-centres when the touch moves away from it.
class BookSide {
 public:
  static constexpr std::uint32_t kMaxWindowLevels = 4096;

  BookSide(Side side, std::uint32_t window_levels);

  void append(Order& order);
  void unlink(Order& order) noexcept;
  void reduce(Order& order, std::uint32_t shares) noexcept;

  [[nodiscard]] BestQuote best() const noexcept;
  [[nodiscard]] std::vector<LevelView> levels() const;
  [[nodiscard]] const SideStats& stats() const noexcept { return stats_; }

  // Grid index: $0.0001 steps below $1.00, $0.01 steps from $1.00 (SEC Rule 612).
  [[nodiscard]] static bool on_grid(std::uint32_t price) noexcept;
  [[nodiscard]] static std::uint32_t grid_index(std::uint32_t price) noexcept;
  [[nodiscard]] static std::uint32_t grid_price(std::uint32_t index) noexcept;

 private:
  [[nodiscard]] bool better(std::uint32_t lhs, std::uint32_t rhs) const noexcept {
    return side_ == Side::bid ? lhs > rhs : lhs < rhs;
  }
  // Slot of `price` in the flat array, or std::nullopt when it lives in the overflow map.
  [[nodiscard]] std::optional<std::uint32_t> window_slot(std::uint32_t price) const noexcept;
  [[nodiscard]] PriceLevel* find(std::uint32_t price) noexcept;
  [[nodiscard]] std::optional<std::uint32_t> window_best() const noexcept;
  [[nodiscard]] bool should_recenter(std::uint32_t price) const noexcept;
  void recenter(std::uint32_t price);
  void mark(std::uint32_t slot) noexcept;
  void unmark(std::uint32_t slot) noexcept;

  Side side_;
  std::uint32_t width_;
  std::uint32_t base_{};  // grid index of window_[0]
  std::uint32_t occupied_{};
  std::vector<PriceLevel> window_;
  std::vector<std::uint64_t> words_;
  std::uint64_t summary_{};
  std::map<std::uint32_t, PriceLevel> overflow_;
  SideStats stats_{};
};

// Open-addressed reference -> order map with linear probing and backward-shift deletion.
class ReferenceMap {
 public:
  ReferenceMap();
  [[nodiscard]] Order* find(std::uint64_t key) const noexcept;
  void insert(std::uint64_t key, Order* value);
  void erase(std::uint64_t key) noexcept;
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  template <typename F>
  void for_each(F&& visit) const {
    for (const auto& entry : entries_) {
      if (entry.value != nullptr) visit(entry.value);
    }
  }

 private:
  struct Entry {
    std::uint64_t key;
    Order* value;
  };
  [[nodiscard]] std::size_t home(std::uint64_t key) const noexcept {
    return static_cast<std::size_t>((key * 0x9E3779B97F4A7C15ull) >> shift_);
  }
  void grow();

  std::vector<Entry> entries_;
  std::size_t mask_{};
  unsigned shift_{};
  std::size_t size_{};
};

}  // namespace detail

// Level 3 book for one instrument: every resting order is tracked individually, in
// price-time priority, by its exchange reference number.
class OrderBook {
 public:
  static constexpr std::uint32_t kDefaultWindowLevels = 1024;

  explicit OrderBook(OrderPool& pool, std::uint32_t window_levels = kDefaultWindowLevels);
  ~OrderBook();
  OrderBook(const OrderBook&) = delete;
  OrderBook& operator=(const OrderBook&) = delete;

  Status add(std::uint64_t reference, Side side, std::uint32_t shares, std::uint32_t price);
  Status execute(std::uint64_t reference, std::uint32_t shares);
  Status cancel(std::uint64_t reference, std::uint32_t shares);
  Status remove(std::uint64_t reference);
  Status replace(std::uint64_t original_reference, std::uint64_t replacement_reference,
                 std::uint32_t shares, std::uint32_t price);

  [[nodiscard]] BestQuote best_bid() const noexcept { return bids_.best(); }
  [[nodiscard]] BestQuote best_ask() const noexcept { return asks_.best(); }
  [[nodiscard]] std::size_t order_count() const noexcept { return references_.size(); }
  [[nodiscard]] const Order* find(std::uint64_t reference) const noexcept {
    return references_.find(reference);
  }
  // Levels from best to worst; walk level->head via Order::next for queue order.
  [[nodiscard]] std::vector<LevelView> depth(Side side) const { return side_for(side).levels(); }
  [[nodiscard]] const SideStats& stats(Side side) const noexcept { return side_for(side).stats(); }

 private:
  [[nodiscard]] detail::BookSide& side_for(Side side) noexcept {
    return side == Side::bid ? bids_ : asks_;
  }
  [[nodiscard]] const detail::BookSide& side_for(Side side) const noexcept {
    return side == Side::bid ? bids_ : asks_;
  }
  Status reduce(std::uint64_t reference, std::uint32_t shares);
  void erase(Order& order) noexcept;

  OrderPool& pool_;
  detail::ReferenceMap references_;
  detail::BookSide bids_;
  detail::BookSide asks_;
};

}  // namespace itch
