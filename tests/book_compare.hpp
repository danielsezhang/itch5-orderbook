#pragma once

#include "itch/order_book.hpp"

#include <sstream>
#include <string>

namespace itch::testing {

// Full Level 3 comparison of two books: the same levels in the same order, and in each level the
// same orders (reference, open shares) in the same FIFO sequence. Returns "" when identical.
inline std::string diff_books(const OrderBook& lhs, const OrderBook& rhs) {
  std::ostringstream out;
  if (lhs.order_count() != rhs.order_count()) {
    out << "order_count " << lhs.order_count() << " vs " << rhs.order_count() << '\n';
  }
  for (const Side side : {Side::bid, Side::ask}) {
    const auto a = lhs.depth(side);
    const auto b = rhs.depth(side);
    const char* name = side == Side::bid ? "bid" : "ask";
    if (a.size() != b.size()) {
      out << name << " levels " << a.size() << " vs " << b.size() << '\n';
      continue;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
      if (a[i].price != b[i].price || a[i].level->shares != b[i].level->shares) {
        out << name << " level " << i << ": " << a[i].price << 'x' << a[i].level->shares << " vs "
            << b[i].price << 'x' << b[i].level->shares << '\n';
        break;
      }
      const Order* x = a[i].level->head;
      const Order* y = b[i].level->head;
      for (; x != nullptr && y != nullptr; x = x->next, y = y->next) {
        if (x->reference != y->reference || x->shares != y->shares) break;
      }
      if (x != nullptr || y != nullptr) {
        out << name << " queue differs at price " << a[i].price << '\n';
        break;
      }
    }
  }
  return out.str();
}

}  // namespace itch::testing
