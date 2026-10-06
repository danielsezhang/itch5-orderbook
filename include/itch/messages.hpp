#pragma once

#include "itch/endian.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace itch {

// Every message type in the Nasdaq TotalView-ITCH 5.0 specification.
enum class MessageType : char {
  system_event = 'S',
  stock_directory = 'R',
  stock_trading_action = 'H',
  reg_sho_restriction = 'Y',
  market_participant_position = 'L',
  mwcb_decline_level = 'V',
  mwcb_status = 'W',
  ipo_quoting_period = 'K',
  luld_auction_collar = 'J',
  operational_halt = 'h',
  add_order = 'A',
  add_order_attributed = 'F',
  order_executed = 'E',
  order_executed_with_price = 'C',
  order_cancel = 'X',
  order_delete = 'D',
  order_replace = 'U',
  trade = 'P',
  cross_trade = 'Q',
  broken_trade = 'B',
  net_order_imbalance = 'I',
  retail_price_improvement = 'N',
  direct_listing_with_capital_raise = 'O',
};

namespace detail {

struct SpecLength {
  MessageType type;
  std::uint8_t bytes;
};

inline constexpr SpecLength kSpecLengths[] = {
    {MessageType::system_event, 12},
    {MessageType::stock_directory, 39},
    {MessageType::stock_trading_action, 25},
    {MessageType::reg_sho_restriction, 20},
    {MessageType::market_participant_position, 26},
    {MessageType::mwcb_decline_level, 35},
    {MessageType::mwcb_status, 12},
    {MessageType::ipo_quoting_period, 28},
    {MessageType::luld_auction_collar, 35},
    {MessageType::operational_halt, 21},
    {MessageType::add_order, 36},
    {MessageType::add_order_attributed, 40},
    {MessageType::order_executed, 31},
    {MessageType::order_executed_with_price, 36},
    {MessageType::order_cancel, 23},
    {MessageType::order_delete, 19},
    {MessageType::order_replace, 35},
    {MessageType::trade, 44},
    {MessageType::cross_trade, 40},
    {MessageType::broken_trade, 19},
    {MessageType::net_order_imbalance, 50},
    {MessageType::retail_price_improvement, 20},
    {MessageType::direct_listing_with_capital_raise, 48},
};

// Indexed by the raw type byte, so the parser's length check is one load. Zero marks a byte
// that is not a defined message type.
inline constexpr std::array<std::uint8_t, 256> kLengthByType = [] {
  std::array<std::uint8_t, 256> table{};
  for (const auto [type, bytes] : kSpecLengths) table[static_cast<unsigned char>(type)] = bytes;
  return table;
}();

}  // namespace detail

// Length in bytes that the specification gives a message type, excluding the 2-byte BinaryFILE
// length prefix; std::nullopt when the byte is not a defined type.
[[nodiscard]] constexpr std::optional<std::size_t> spec_length(char type) noexcept {
  const std::uint8_t bytes = detail::kLengthByType[static_cast<unsigned char>(type)];
  if (bytes == 0) return std::nullopt;
  return bytes;
}

[[nodiscard]] constexpr std::size_t spec_length(MessageType type) noexcept {
  return detail::kLengthByType[static_cast<unsigned char>(type)];
}

// Wire layouts from the Nasdaq TotalView-ITCH 5.0 specification. Fields stay big-endian in the
// mapped file; accessors byte-swap on read so decoding never copies a message.

struct [[gnu::packed]] MessageHeader {
  char type;
  std::uint16_t stock_locate;
  std::uint16_t tracking_number;
  std::uint8_t timestamp[6];

  [[nodiscard]] std::uint16_t locate() const noexcept { return be16(stock_locate); }
  [[nodiscard]] std::uint64_t timestamp_ns() const noexcept { return be48(timestamp); }
};

// 'A' (36 bytes); 'F' (40 bytes) is the same layout followed by a 4-byte MPID.
struct [[gnu::packed]] AddOrder {
  static constexpr MessageType kType = MessageType::add_order;

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

// 'E': executed at the resting order's price.
struct [[gnu::packed]] ExecuteOrder {
  static constexpr MessageType kType = MessageType::order_executed;

  MessageHeader header;
  std::uint64_t reference;
  std::uint32_t shares;
  std::uint64_t match_number;

  [[nodiscard]] std::uint64_t order_reference() const noexcept { return be64(reference); }
  [[nodiscard]] std::uint32_t share_count() const noexcept { return be32(shares); }
};

// 'C': executed at a price other than the order's displayed price (for example in a cross).
struct [[gnu::packed]] ExecuteWithPrice {
  static constexpr MessageType kType = MessageType::order_executed_with_price;

  MessageHeader header;
  std::uint64_t reference;
  std::uint32_t shares;
  std::uint64_t match_number;
  char printable;
  std::uint32_t price;

  [[nodiscard]] std::uint64_t order_reference() const noexcept { return be64(reference); }
  [[nodiscard]] std::uint32_t share_count() const noexcept { return be32(shares); }
  [[nodiscard]] std::uint32_t price_ticks() const noexcept { return be32(price); }
};

// 'X': partial cancel.
struct [[gnu::packed]] CancelOrder {
  static constexpr MessageType kType = MessageType::order_cancel;

  MessageHeader header;
  std::uint64_t reference;
  std::uint32_t cancelled_shares;

  [[nodiscard]] std::uint64_t order_reference() const noexcept { return be64(reference); }
  [[nodiscard]] std::uint32_t share_count() const noexcept { return be32(cancelled_shares); }
};

// 'D': full delete.
struct [[gnu::packed]] DeleteOrder {
  static constexpr MessageType kType = MessageType::order_delete;

  MessageHeader header;
  std::uint64_t reference;

  [[nodiscard]] std::uint64_t order_reference() const noexcept { return be64(reference); }
};

// 'U': cancel-replace; the replacement keeps the side but loses time priority.
struct [[gnu::packed]] ReplaceOrder {
  static constexpr MessageType kType = MessageType::order_replace;

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

// True for the message types that change a Level 3 book.
[[nodiscard]] constexpr bool is_order_message(char type) noexcept {
  switch (static_cast<MessageType>(type)) {
    case MessageType::add_order:
    case MessageType::add_order_attributed:
    case MessageType::order_executed:
    case MessageType::order_executed_with_price:
    case MessageType::order_cancel:
    case MessageType::order_delete:
    case MessageType::order_replace:
      return true;
    default:
      return false;
  }
}

// The packed layouts must match the specification byte for byte, or every field read is wrong.
static_assert(sizeof(MessageHeader) == 11);
static_assert(sizeof(AddOrder) == spec_length(MessageType::add_order));
static_assert(sizeof(AddOrder) + 4 == spec_length(MessageType::add_order_attributed));
static_assert(sizeof(ExecuteOrder) == spec_length(MessageType::order_executed));
static_assert(sizeof(ExecuteWithPrice) == spec_length(MessageType::order_executed_with_price));
static_assert(sizeof(CancelOrder) == spec_length(MessageType::order_cancel));
static_assert(sizeof(DeleteOrder) == spec_length(MessageType::order_delete));
static_assert(sizeof(ReplaceOrder) == spec_length(MessageType::order_replace));

}  // namespace itch
