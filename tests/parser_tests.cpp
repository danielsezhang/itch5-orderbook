#include "itch/parser.hpp"

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstring>
#include <vector>

namespace {
void append_u16(std::vector<std::byte>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::byte>(value >> 8));
  bytes.push_back(static_cast<std::byte>(value));
}
template <typename Message>
void append(std::vector<std::byte>& bytes, const Message& message) {
  append_u16(bytes, sizeof(Message));
  const auto* raw = reinterpret_cast<const std::byte*>(&message);
  bytes.insert(bytes.end(), raw, raw + sizeof(Message));
}
}  // namespace

TEST_CASE("parser walks length-prefixed binary ITCH messages without copies") {
  itch::AddOrder add{};
  add.header.type = 'A';
  add.reference = __builtin_bswap64(42);
  add.side = 'B';
  add.shares = __builtin_bswap32(100);
  add.price = __builtin_bswap32(12'345);
  itch::CancelOrder cancel{};
  cancel.header.type = 'X';
  cancel.reference = __builtin_bswap64(42);
  cancel.cancelled_shares = __builtin_bswap32(100);

  std::vector<std::byte> bytes;
  append(bytes, add);
  append(bytes, cancel);
  itch::Parser parser(bytes.data(), bytes.size());
  std::uint64_t reference{};
  std::uint32_t price{};
  std::uint32_t cancelled{};
  parser.parse(
      [&](const itch::AddOrder& message) { reference = message.order_reference(); price = message.price_ticks(); },
      [](const itch::ExecuteOrder&) {},
      [&](const itch::CancelOrder& message) { cancelled = message.share_count(); },
      [](const itch::ReplaceOrder&) {});

  REQUIRE(reference == 42);
  REQUIRE(price == 12'345);
  REQUIRE(cancelled == 100);
}
