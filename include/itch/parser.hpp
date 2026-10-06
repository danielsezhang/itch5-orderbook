#pragma once

#include "itch/endian.hpp"
#include "itch/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <stdexcept>

namespace itch {

// A visitor implements any subset of on(const AddOrder&), on(const ExecuteOrder&),
// on(const ExecuteWithPrice&), on(const CancelOrder&), on(const DeleteOrder&) and
// on(const ReplaceOrder&). Overloads it lacks are skipped at compile time.
template <typename Message, typename Visitor>
void deliver(const std::byte* message, Visitor& visitor) {
  if constexpr (requires(const Message& m) { visitor.on(m); }) {
    visitor.on(*reinterpret_cast<const Message*>(message));
  }
}

// Hands one order-book message to the visitor as a view into `message`; other types are ignored.
template <typename Visitor>
void dispatch_order_message(const std::byte* message, Visitor& visitor) {
  switch (static_cast<MessageType>(*message)) {
    case MessageType::add_order:
    case MessageType::add_order_attributed:
      deliver<AddOrder>(message, visitor);
      break;
    case MessageType::order_executed:
      deliver<ExecuteOrder>(message, visitor);
      break;
    case MessageType::order_executed_with_price:
      deliver<ExecuteWithPrice>(message, visitor);
      break;
    case MessageType::order_cancel:
      deliver<CancelOrder>(message, visitor);
      break;
    case MessageType::order_delete:
      deliver<DeleteOrder>(message, visitor);
      break;
    case MessageType::order_replace:
      deliver<ReplaceOrder>(message, visitor);
      break;
    default:
      break;
  }
}

// Walks a Nasdaq BinaryFILE stream, where each message is preceded by a 2-byte big-endian
// length, without copying it.
class Parser {
 public:
  Parser(const std::byte* data, std::size_t size) : begin_(data), end_(data + size) {}

  // Calls `on_message(const std::byte*)` for every message and returns how many there were.
  // Throws on truncation or on a length that contradicts the specification, since either means
  // the stream is no longer aligned on message boundaries.
  template <typename OnMessage>
  std::uint64_t for_each_message(OnMessage&& on_message) const {
    std::uint64_t count = 0;
    for (const std::byte* cursor = begin_; cursor != end_; ++count) {
      if (end_ - cursor < kPrefixBytes) throw std::runtime_error("truncated ITCH length prefix");
      std::uint16_t wire_length;
      std::memcpy(&wire_length, cursor, sizeof(wire_length));
      const std::size_t length = be16(wire_length);
      cursor += kPrefixBytes;
      if (length == 0 || static_cast<std::size_t>(end_ - cursor) < length) {
        throw std::runtime_error("truncated ITCH message");
      }
      const auto expected = spec_length(static_cast<char>(*cursor));
      if (expected && *expected != length) {
        throw std::runtime_error("ITCH message length does not match its type");
      }
      on_message(cursor);
      cursor += length;
    }
    return count;
  }

  // Parses the whole stream into the visitor and returns the number of messages walked.
  template <typename Visitor>
  std::uint64_t parse(Visitor& visitor) const {
    return for_each_message(
        [&visitor](const std::byte* message) { dispatch_order_message(message, visitor); });
  }

 private:
  static constexpr std::ptrdiff_t kPrefixBytes = 2;

  const std::byte* begin_;
  const std::byte* end_;
};

}  // namespace itch
