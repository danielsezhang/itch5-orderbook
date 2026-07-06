#pragma once

#include <cstdint>

namespace itch {

enum class Side : char { bid = 'B', ask = 'S' };

// Resting order. prev/next form the intrusive FIFO list of its price level, so a level needs
// no separate node allocation and a cancel unlinks in O(1).
struct Order {
  std::uint64_t reference;
  std::uint32_t shares;
  std::uint32_t price;
  Order* prev;
  Order* next;
  Side side;
};

}  // namespace itch
