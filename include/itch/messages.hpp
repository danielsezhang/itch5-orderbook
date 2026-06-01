#pragma once

#include "itch/endian.hpp"

#include <cstdint>

namespace itch {

struct [[gnu::packed]] MessageHeader {
  char type;
  std::uint16_t stock_locate;
  std::uint16_t tracking_number;
  std::uint8_t timestamp[6];

  [[nodiscard]] std::uint64_t timestamp_ns() const noexcept { return be48(timestamp); }
};

struct [[gnu::packed]] AddOrder {
  MessageHeader header;
  std::uint64_t reference;
  char side;
  std::uint32_t shares;
  char stock[8];
  std::uint32_t price;

  [[nodiscard]] std::uint64_t order_reference() const noexcept { return be64(reference); }
  [[nodiscard]] std::uint32_t share_count() const noexcept { return be32(shares); }
  [[nodiscard]] std::uint32_t price_ticks() const noexcept { return be32(price); }
};

struct [[gnu::packed]] ExecuteOrder {
  MessageHeader header;
  std::uint64_t reference;
  std::uint32_t shares;
  std::uint64_t match_number;

  [[nodiscard]] std::uint64_t order_reference() const noexcept { return be64(reference); }
  [[nodiscard]] std::uint32_t share_count() const noexcept { return be32(shares); }
};

struct [[gnu::packed]] CancelOrder {
  MessageHeader header;
  std::uint64_t reference;
  std::uint32_t cancelled_shares;

  [[nodiscard]] std::uint64_t order_reference() const noexcept { return be64(reference); }
  [[nodiscard]] std::uint32_t share_count() const noexcept { return be32(cancelled_shares); }
};

struct [[gnu::packed]] ReplaceOrder {
  MessageHeader header;
  std::uint64_t original_reference;
  std::uint64_t replacement_reference;
  std::uint32_t shares;
  std::uint32_t price;

  [[nodiscard]] std::uint64_t original_order_reference() const noexcept {
    return be64(original_reference);
  }
  [[nodiscard]] std::uint64_t replacement_order_reference() const noexcept {
    return be64(replacement_reference);
  }
  [[nodiscard]] std::uint32_t share_count() const noexcept { return be32(shares); }
  [[nodiscard]] std::uint32_t price_ticks() const noexcept { return be32(price); }
};

static_assert(sizeof(AddOrder) == 36);
static_assert(sizeof(ExecuteOrder) == 31);
static_assert(sizeof(CancelOrder) == 23);
static_assert(sizeof(ReplaceOrder) == 35);

}  // namespace itch
