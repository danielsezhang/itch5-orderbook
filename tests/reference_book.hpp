#pragma once

// Deliberately naive Level 3 book used as an oracle: std::map levels, std::list FIFO queues.

#include "itch/order_book.hpp"

#include <cstdint>
#include <functional>
#include <list>
#include <map>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace itch::testing {

class ReferenceBook {
 public:
  struct Resting {
    std::uint64_t reference;
    std::uint32_t shares;
  };
  using Queue = std::list<Resting>;

  Status add(std::uint64_t reference, Side side, std::uint32_t shares, std::uint32_t price) {
    if (shares == 0) return Status::invalid;
    if (index_.count(reference) != 0) return Status::duplicate_reference;
    Queue& queue = side == Side::bid ? bids_[price] : asks_[price];
    queue.push_back({reference, shares});
    index_[reference] = {side, price, std::prev(queue.end())};
    return Status::ok;
  }

  Status reduce(std::uint64_t reference, std::uint32_t shares) {
    const auto found = index_.find(reference);
    if (found == index_.end()) return Status::unknown_reference;
    auto& resting = *found->second.position;
    if (shares >= resting.shares) {
      const bool exceeds = shares > resting.shares;
      erase(found);
      return exceeds ? Status::exceeds_open_shares : Status::ok;
    }
    resting.shares -= shares;
    return Status::ok;
  }

  Status remove(std::uint64_t reference) {
    const auto found = index_.find(reference);
    if (found == index_.end()) return Status::unknown_reference;
    erase(found);
    return Status::ok;
  }

  Status replace(std::uint64_t original, std::uint64_t replacement, std::uint32_t shares,
                 std::uint32_t price) {
    const auto found = index_.find(original);
    if (found == index_.end()) return Status::unknown_reference;
    if (shares == 0) return Status::invalid;
    if (replacement != original && index_.count(replacement) != 0) {
      return Status::duplicate_reference;
    }
    const Side side = found->second.side;
    erase(found);
    return add(replacement, side, shares, price);
  }

  [[nodiscard]] BestQuote best(Side side) const {
    if (side == Side::bid) return bids_.empty() ? BestQuote{} : quote(*bids_.begin());
    return asks_.empty() ? BestQuote{} : quote(*asks_.begin());
  }

  [[nodiscard]] std::size_t order_count() const { return index_.size(); }

  // Full Level 3 comparison: identical levels, queue order, references, and open shares.
  [[nodiscard]] std::string diff(const OrderBook& book) const {
    std::ostringstream out;
    if (book.order_count() != order_count()) {
      out << "order_count fast=" << book.order_count() << " ref=" << order_count() << '\n';
    }
    compare(out, "bid", book.depth(Side::bid), bids_);
    compare(out, "ask", book.depth(Side::ask), asks_);
    return out.str();
  }

 private:
  struct Location {
    Side side;
    std::uint32_t price;
    Queue::iterator position;
  };
  using Index = std::unordered_map<std::uint64_t, Location>;

  template <typename Levels>
  static void compare(std::ostringstream& out, const char* name, const std::vector<LevelView>& fast,
                      const Levels& expected) {
    if (fast.size() != expected.size()) {
      out << name << " levels fast=" << fast.size() << " ref=" << expected.size() << '\n';
      return;
    }
    auto level = expected.begin();
    for (const LevelView& view : fast) {
      const auto& [price, queue] = *level++;
      std::uint64_t total = 0;
      for (const auto& resting : queue) total += resting.shares;
      if (view.price != price || view.level->shares != total) {
        out << name << " level fast=" << view.price << 'x' << view.level->shares << " ref=" << price
            << 'x' << total << '\n';
        return;
      }
      const Order* order = view.level->head;
      for (const auto& resting : queue) {
        if (order == nullptr || order->reference != resting.reference ||
            order->shares != resting.shares || order->price != price) {
          out << name << " queue mismatch at price " << price << '\n';
          return;
        }
        order = order->next;
      }
      if (order != nullptr) out << name << " extra order at price " << price << '\n';
    }
  }

  template <typename Entry>
  static BestQuote quote(const Entry& entry) {
    std::uint64_t total = 0;
    for (const auto& resting : entry.second) total += resting.shares;
    return {entry.first, total};
  }

  void erase(Index::iterator found) {
    const auto [side, price, position] = found->second;
    if (side == Side::bid) {
      auto level = bids_.find(price);
      level->second.erase(position);
      if (level->second.empty()) bids_.erase(level);
    } else {
      auto level = asks_.find(price);
      level->second.erase(position);
      if (level->second.empty()) asks_.erase(level);
    }
    index_.erase(found);
  }

  std::map<std::uint32_t, Queue, std::greater<>> bids_;
  std::map<std::uint32_t, Queue> asks_;
  Index index_;
};

}  // namespace itch::testing
