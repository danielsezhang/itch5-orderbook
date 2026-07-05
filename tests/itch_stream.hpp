#pragma once

// Encodes ITCH 5.0 messages, big-endian with BinaryFILE length prefixes, for tests.

#include "itch/messages.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace itch::testing {

class ItchStream {
 public:
  ItchStream& add(std::uint16_t locate, std::uint64_t reference, char side, std::uint32_t shares,
                  std::uint32_t price, bool attributed = false) {
    auto m = start<AddOrder>(locate);
    if (attributed) m.header.type = static_cast<char>(MessageType::add_order_attributed);
    m.reference = be64(reference);
    m.side = side;
    m.shares = be32(shares);
    m.price = be32(price);
    append(m, attributed ? spec_length(MessageType::add_order_attributed) : sizeof(m));
    return *this;
  }

  ItchStream& execute(std::uint16_t locate, std::uint64_t reference, std::uint32_t shares) {
    auto m = start<ExecuteOrder>(locate);
    m.reference = be64(reference);
    m.shares = be32(shares);
    return append(m);
  }

  ItchStream& execute_with_price(std::uint16_t locate, std::uint64_t reference,
                                 std::uint32_t shares, std::uint32_t price) {
    auto m = start<ExecuteWithPrice>(locate);
    m.reference = be64(reference);
    m.shares = be32(shares);
    m.price = be32(price);
    return append(m);
  }

  ItchStream& cancel(std::uint16_t locate, std::uint64_t reference, std::uint32_t shares) {
    auto m = start<CancelOrder>(locate);
    m.reference = be64(reference);
    m.cancelled_shares = be32(shares);
    return append(m);
  }

  ItchStream& remove(std::uint16_t locate, std::uint64_t reference) {
    auto m = start<DeleteOrder>(locate);
    m.reference = be64(reference);
    return append(m);
  }

  ItchStream& replace(std::uint16_t locate, std::uint64_t original, std::uint64_t replacement,
                      std::uint32_t shares, std::uint32_t price) {
    auto m = start<ReplaceOrder>(locate);
    m.original_reference = be64(original);
    m.replacement_reference = be64(replacement);
    m.shares = be32(shares);
    m.price = be32(price);
    return append(m);
  }

  // A message type the book ignores, to check it is skipped without losing alignment.
  ItchStream& system_event() {
    std::array<char, spec_length(MessageType::system_event)> m{};
    m[0] = static_cast<char>(MessageType::system_event);
    return append(m);
  }

  // Raw message with an explicit length prefix, for malformed-input tests.
  template <typename Message>
  ItchStream& append(const Message& message, std::size_t length = sizeof(Message)) {
    bytes_.push_back(static_cast<std::byte>(length >> 8));
    bytes_.push_back(static_cast<std::byte>(length));
    const auto* raw = reinterpret_cast<const std::byte*>(&message);
    bytes_.insert(bytes_.end(), raw, raw + std::min(length, sizeof(Message)));
    if (length > sizeof(Message)) bytes_.insert(bytes_.end(), length - sizeof(Message), std::byte{0});
    ++messages_;
    return *this;
  }

  [[nodiscard]] const std::vector<std::byte>& bytes() const noexcept { return bytes_; }
  [[nodiscard]] const std::byte* data() const noexcept { return bytes_.data(); }
  [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }
  [[nodiscard]] std::uint64_t messages() const noexcept { return messages_; }

 private:
  template <typename Message>
  static Message start(std::uint16_t locate) {
    Message m{};
    m.header.type = static_cast<char>(Message::kType);
    m.header.stock_locate = be16(locate);
    return m;
  }

  std::vector<std::byte> bytes_;
  std::uint64_t messages_{};
};

}  // namespace itch::testing
