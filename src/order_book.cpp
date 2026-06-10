#include "itch/order_book.hpp"

#include "itch/order_pool.hpp"

#include <algorithm>
#include <stdexcept>

namespace itch {

class OrderBook::ReferenceMap {
 public:
  explicit ReferenceMap(std::size_t capacity = 1024) : entries_(capacity) {}

  void insert(std::uint64_t key, Order* value) {
    if ((size_ + 1) * 10 >= entries_.size() * 7) rehash(entries_.size() * 2);
    auto index = slot(key);
    while (entries_[index].occupied) index = (index + 1) % entries_.size();
    entries_[index] = {key, value, true};
    ++size_;
  }

  Order* find(std::uint64_t key) const {
    auto index = slot(key);
    for (;;) {
      const auto& entry = entries_[index];
      if (!entry.occupied) return nullptr;
      if (entry.key == key) return entry.value;
      index = (index + 1) % entries_.size();
    }
  }

  void erase(std::uint64_t key) {
    auto index = slot(key);
    while (entries_[index].occupied && entries_[index].key != key) {
      index = (index + 1) % entries_.size();
    }
    if (!entries_[index].occupied) return;
    entries_[index].occupied = false;
    --size_;
    auto next = (index + 1) % entries_.size();
    while (entries_[next].occupied) {
      auto entry = entries_[next];
      entries_[next].occupied = false;
      --size_;
      insert(entry.key, entry.value);
      next = (next + 1) % entries_.size();
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  struct Entry { std::uint64_t key{}; Order* value{}; bool occupied{}; };
  [[nodiscard]] std::size_t slot(std::uint64_t key) const noexcept {
    return (key * 11400714819323198485ull) % entries_.size();
  }
  void rehash(std::size_t capacity) {
    auto old = std::move(entries_);
    entries_.assign(capacity, {});
    size_ = 0;
    for (const auto& entry : old) if (entry.occupied) insert(entry.key, entry.value);
  }

  std::vector<Entry> entries_;
  std::size_t size_{};
};

OrderBook::OrderBook(std::size_t max_price_ticks)
    : pool_(std::make_unique<OrderPool>()),
      references_(std::make_unique<ReferenceMap>()),
      bids_(max_price_ticks + 1),
      asks_(max_price_ticks + 1) {}

OrderBook::~OrderBook() = default;

void OrderBook::add(std::uint64_t reference, Side side, std::uint32_t shares, std::uint32_t price) {
  if (price >= bids_.size() || shares == 0 || references_->find(reference) != nullptr) {
    throw std::invalid_argument("invalid or duplicate order");
  }
  auto* order = pool_->create(reference, side, shares, price);
  auto& level = level_for(*order);
  order->prev = level.tail;
  if (level.tail) level.tail->next = order; else level.head = order;
  level.tail = order;
  level.displayed_shares += shares;
  references_->insert(reference, order);
  if (side == Side::bid && (!best_bid_.valid() || price >= best_bid_.price)) best_bid_ = {price, level.displayed_shares};
  if (side == Side::ask && (!best_ask_.valid() || price <= best_ask_.price)) best_ask_ = {price, level.displayed_shares};
}

void OrderBook::execute(std::uint64_t reference, std::uint32_t shares) {
  if (auto* order = references_->find(reference)) reduce(*order, shares);
}

void OrderBook::cancel(std::uint64_t reference, std::uint32_t shares) {
  if (auto* order = references_->find(reference)) reduce(*order, shares);
}

void OrderBook::replace(std::uint64_t original_reference, std::uint64_t replacement_reference,
                        std::uint32_t shares, std::uint32_t price) {
  auto* original = references_->find(original_reference);
  if (original == nullptr) return;
  const auto side = original->side;
  remove(*original);
  add(replacement_reference, side, shares, price);
}

BestQuote OrderBook::best_bid() const noexcept { return best_bid_; }
BestQuote OrderBook::best_ask() const noexcept { return best_ask_; }
std::size_t OrderBook::order_count() const noexcept { return references_->size(); }

void OrderBook::reduce(Order& order, std::uint32_t shares) {
  if (shares >= order.shares) {
    remove(order);
    return;
  }
  auto& level = level_for(order);
  order.shares -= shares;
  level.displayed_shares -= shares;
  if (order.side == Side::bid && order.price == best_bid_.price) best_bid_.shares = level.displayed_shares;
  if (order.side == Side::ask && order.price == best_ask_.price) best_ask_.shares = level.displayed_shares;
}

void OrderBook::remove(Order& order) {
  auto& level = level_for(order);
  if (order.prev) order.prev->next = order.next; else level.head = order.next;
  if (order.next) order.next->prev = order.prev; else level.tail = order.prev;
  level.displayed_shares -= order.shares;
  const auto side = order.side;
  const auto price = order.price;
  references_->erase(order.reference);
  pool_->destroy(&order);
  if ((side == Side::bid && best_bid_.price == price) ||
      (side == Side::ask && best_ask_.price == price)) refresh_best(side);
}

PriceLevel& OrderBook::level_for(const Order& order) noexcept {
  return (order.side == Side::bid ? bids_ : asks_)[order.price];
}
const PriceLevel& OrderBook::level_for(const Order& order) const noexcept {
  return (order.side == Side::bid ? bids_ : asks_)[order.price];
}

void OrderBook::refresh_best(Side side) {
  const auto& levels = side == Side::bid ? bids_ : asks_;
  if (side == Side::bid) {
    for (std::size_t price = levels.size(); price-- > 0;) {
      if (!levels[price].empty()) { best_bid_ = {static_cast<std::uint32_t>(price), levels[price].displayed_shares}; return; }
    }
    best_bid_ = {};
  } else {
    for (std::size_t price = 0; price < levels.size(); ++price) {
      if (!levels[price].empty()) { best_ask_ = {static_cast<std::uint32_t>(price), levels[price].displayed_shares}; return; }
    }
    best_ask_ = {};
  }
}

}  // namespace itch
