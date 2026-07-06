#include "itch/order_book.hpp"

#include <algorithm>
#include <bit>
#include <stdexcept>

namespace itch {
namespace detail {
namespace {
constexpr std::uint32_t kDollar = 10'000;
constexpr std::uint32_t kCent = 100;
}  // namespace

BookSide::BookSide(Side side, std::uint32_t window_levels) : side_(side), width_(window_levels) {
  if (!std::has_single_bit(window_levels) || window_levels < 64 ||
      window_levels > kMaxWindowLevels) {
    throw std::invalid_argument("window_levels must be a power of two in [64, 4096]");
  }
}

bool BookSide::on_grid(std::uint32_t price) noexcept {
  return price < kDollar || price % kCent == 0;
}

std::uint32_t BookSide::grid_index(std::uint32_t price) noexcept {
  return price < kDollar ? price : kDollar + (price - kDollar) / kCent;
}

std::uint32_t BookSide::grid_price(std::uint32_t index) noexcept {
  return index < kDollar ? index : kDollar + (index - kDollar) * kCent;
}

std::optional<std::uint32_t> BookSide::window_slot(std::uint32_t price) const noexcept {
  if (window_.empty() || !on_grid(price)) return std::nullopt;
  const std::uint32_t index = grid_index(price);
  if (index < base_ || index - base_ >= width_) return std::nullopt;
  return index - base_;
}

PriceLevel* BookSide::find(std::uint32_t price) noexcept {
  if (const auto slot = window_slot(price)) return &window_[*slot];
  const auto found = overflow_.find(price);
  return found == overflow_.end() ? nullptr : &found->second;
}

void BookSide::mark(std::uint32_t slot) noexcept {
  words_[slot >> 6] |= std::uint64_t{1} << (slot & 63);
  summary_ |= std::uint64_t{1} << (slot >> 6);
  ++occupied_;
}

void BookSide::unmark(std::uint32_t slot) noexcept {
  auto& word = words_[slot >> 6];
  word &= ~(std::uint64_t{1} << (slot & 63));
  if (word == 0) summary_ &= ~(std::uint64_t{1} << (slot >> 6));
  --occupied_;
}

// Highest set bit for bids, lowest for asks: one instruction on the summary word picks the
// 64-level block, a second picks the level inside it.
std::optional<std::uint32_t> BookSide::window_best() const noexcept {
  if (summary_ == 0) return std::nullopt;
  if (side_ == Side::bid) {
    const unsigned word = 63 - std::countl_zero(summary_);
    return word * 64 + (63 - std::countl_zero(words_[word]));
  }
  const unsigned word = std::countr_zero(summary_);
  return word * 64 + std::countr_zero(words_[word]);
}

BestQuote BookSide::best() const noexcept {
  BestQuote quote{};
  if (const auto slot = window_best()) quote = {grid_price(base_ + *slot), window_[*slot].shares};
  if (!overflow_.empty()) {
    const auto& [price, level] = side_ == Side::bid ? *overflow_.rbegin() : *overflow_.begin();
    if (!quote.valid() || better(price, quote.price)) quote = {price, level.shares};
  }
  return quote;
}

bool BookSide::should_recenter(std::uint32_t price) const noexcept {
  if (!on_grid(price)) return false;
  if (occupied_ == 0) return true;
  const BestQuote current = best();
  if (better(price, current.price)) return true;
  return !window_slot(current.price).has_value();
}

void BookSide::recenter(std::uint32_t price) {
  if (window_.empty()) {
    window_.resize(width_);
    words_.resize(width_ / 64);
  }
  for (std::uint32_t word = 0; word < words_.size(); ++word) {
    for (std::uint64_t bits = words_[word]; bits != 0; bits &= bits - 1) {
      const std::uint32_t slot = word * 64 + std::countr_zero(bits);
      overflow_.emplace(grid_price(base_ + slot), window_[slot]);
      window_[slot] = {};
    }
    words_[word] = 0;
  }
  summary_ = 0;
  occupied_ = 0;

  // Most activity sits behind the touch, so leave a quarter of the window in front of it.
  const std::uint32_t index = grid_index(price);
  const std::uint32_t ahead = width_ / 4;
  const std::uint32_t behind = width_ - ahead - 1;
  if (side_ == Side::bid) {
    base_ = index > behind ? index - behind : 0;
  } else {
    base_ = index > ahead ? index - ahead : 0;
  }

  auto it = overflow_.lower_bound(grid_price(base_));
  const std::uint32_t last = grid_price(base_ + width_ - 1);
  while (it != overflow_.end() && it->first <= last) {
    if (const auto slot = window_slot(it->first)) {
      window_[*slot] = it->second;
      mark(*slot);
      it = overflow_.erase(it);
    } else {
      ++it;
    }
  }
  ++stats_.recenters;
}

void BookSide::append(Order& order) {
  auto slot = window_slot(order.price);
  if (!slot && should_recenter(order.price)) {
    recenter(order.price);
    slot = window_slot(order.price);
  }
  PriceLevel* level;
  if (slot) {
    level = &window_[*slot];
    if (level->empty()) mark(*slot);
    ++stats_.window_inserts;
  } else {
    level = &overflow_[order.price];
    ++stats_.overflow_inserts;
  }

  order.prev = level->tail;
  order.next = nullptr;
  if (level->tail != nullptr) {
    level->tail->next = &order;
  } else {
    level->head = &order;
  }
  level->tail = &order;
  level->shares += order.shares;
}

void BookSide::unlink(Order& order) noexcept {
  const auto slot = window_slot(order.price);
  PriceLevel& level = slot ? window_[*slot] : overflow_.find(order.price)->second;
  if (order.prev != nullptr) order.prev->next = order.next; else level.head = order.next;
  if (order.next != nullptr) order.next->prev = order.prev; else level.tail = order.prev;
  level.shares -= order.shares;
  if (!level.empty()) return;
  if (slot) {
    unmark(*slot);
  } else {
    overflow_.erase(order.price);
  }
}

void BookSide::reduce(Order& order, std::uint32_t shares) noexcept {
  find(order.price)->shares -= shares;
  order.shares -= shares;
}

std::vector<LevelView> BookSide::levels() const {
  std::vector<LevelView> out;
  for (std::uint32_t word = 0; word < words_.size(); ++word) {
    for (std::uint64_t bits = words_[word]; bits != 0; bits &= bits - 1) {
      const std::uint32_t slot = word * 64 + std::countr_zero(bits);
      out.push_back({grid_price(base_ + slot), &window_[slot]});
    }
  }
  for (const auto& [price, level] : overflow_) out.push_back({price, &level});
  std::sort(out.begin(), out.end(), [this](const LevelView& lhs, const LevelView& rhs) {
    return better(lhs.price, rhs.price);
  });
  return out;
}

ReferenceMap::ReferenceMap() : entries_(64), mask_(63), shift_(64 - 6) {}

Order* ReferenceMap::find(std::uint64_t key) const noexcept {
  for (std::size_t index = home(key);; index = (index + 1) & mask_) {
    const Entry& entry = entries_[index];
    if (entry.value == nullptr) return nullptr;
    if (entry.key == key) return entry.value;
  }
}

void ReferenceMap::insert(std::uint64_t key, Order* value) {
  if ((size_ + 1) * 2 > entries_.size()) grow();
  std::size_t index = home(key);
  while (entries_[index].value != nullptr) index = (index + 1) & mask_;
  entries_[index] = {key, value};
  ++size_;
}

void ReferenceMap::erase(std::uint64_t key) noexcept {
  std::size_t hole = home(key);
  while (entries_[hole].key != key || entries_[hole].value == nullptr) {
    if (entries_[hole].value == nullptr) return;
    hole = (hole + 1) & mask_;
  }
  // Shift later members of the probe run back so lookups never stop at a false gap.
  for (std::size_t next = (hole + 1) & mask_; entries_[next].value != nullptr;
       next = (next + 1) & mask_) {
    const std::size_t want = home(entries_[next].key);
    if (((next - want) & mask_) >= ((next - hole) & mask_)) {
      entries_[hole] = entries_[next];
      hole = next;
    }
  }
  entries_[hole] = {};
  --size_;
}

void ReferenceMap::grow() {
  std::vector<Entry> old(entries_.size() * 2);
  old.swap(entries_);
  mask_ = entries_.size() - 1;
  --shift_;
  size_ = 0;
  for (const Entry& entry : old) {
    if (entry.value != nullptr) insert(entry.key, entry.value);
  }
}

}  // namespace detail

OrderBook::OrderBook(OrderPool& pool, std::uint32_t window_levels)
    : pool_(pool), bids_(Side::bid, window_levels), asks_(Side::ask, window_levels) {}

OrderBook::~OrderBook() {
  references_.for_each([this](Order* order) { pool_.release(order); });
}

Status OrderBook::add(std::uint64_t reference, Side side, std::uint32_t shares,
                      std::uint32_t price) {
  if (shares == 0 || (side != Side::bid && side != Side::ask)) return Status::invalid;
  if (references_.find(reference) != nullptr) return Status::duplicate_reference;
  Order* order = ::new (pool_.allocate()) Order{reference, shares, price, nullptr, nullptr, side};
  side_for(side).append(*order);
  references_.insert(reference, order);
  return Status::ok;
}

// An execution and a partial cancel change the book identically; they are separate entry
// points because they are separate events on the feed.
Status OrderBook::execute(std::uint64_t reference, std::uint32_t shares) {
  return reduce(reference, shares);
}

Status OrderBook::cancel(std::uint64_t reference, std::uint32_t shares) {
  return reduce(reference, shares);
}

Status OrderBook::remove(std::uint64_t reference) {
  Order* order = references_.find(reference);
  if (order == nullptr) return Status::unknown_reference;
  erase(*order);
  return Status::ok;
}

Status OrderBook::replace(std::uint64_t original_reference, std::uint64_t replacement_reference,
                          std::uint32_t shares, std::uint32_t price) {
  Order* original = references_.find(original_reference);
  if (original == nullptr) return Status::unknown_reference;
  if (shares == 0) return Status::invalid;
  if (replacement_reference != original_reference &&
      references_.find(replacement_reference) != nullptr) {
    return Status::duplicate_reference;
  }
  const Side side = original->side;
  erase(*original);
  return add(replacement_reference, side, shares, price);
}

Status OrderBook::reduce(std::uint64_t reference, std::uint32_t shares) {
  Order* order = references_.find(reference);
  if (order == nullptr) return Status::unknown_reference;
  if (shares >= order->shares) {
    const bool exceeds = shares > order->shares;
    erase(*order);
    return exceeds ? Status::exceeds_open_shares : Status::ok;
  }
  side_for(order->side).reduce(*order, shares);
  return Status::ok;
}

void OrderBook::erase(Order& order) noexcept {
  side_for(order.side).unlink(order);
  references_.erase(order.reference);
  pool_.release(&order);
}

}  // namespace itch
