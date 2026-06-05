#include "itch/parser.hpp"

#include "itch/endian.hpp"

#include <cstring>
#include <stdexcept>

namespace itch {

void Parser::parse(const AddHandler& on_add, const ExecuteHandler& on_execute,
                   const CancelHandler& on_cancel, const ReplaceHandler& on_replace) const {
  auto* cursor = current_;
  while (cursor != end_) {
    if (end_ - cursor < 2) throw std::runtime_error("truncated ITCH length prefix");
    std::uint16_t wire_length;
    std::memcpy(&wire_length, cursor, sizeof(wire_length));
    const auto length = be16(wire_length);
    cursor += 2;
    if (length == 0 || end_ - cursor < length) throw std::runtime_error("truncated ITCH message");

    switch (static_cast<char>(*cursor)) {
      case 'A':
      case 'F':
        if (length >= sizeof(AddOrder)) on_add(*reinterpret_cast<const AddOrder*>(cursor));
        break;
      case 'E':
      case 'C':
        if (length >= sizeof(ExecuteOrder)) on_execute(*reinterpret_cast<const ExecuteOrder*>(cursor));
        break;
      case 'X':
        if (length >= sizeof(CancelOrder)) on_cancel(*reinterpret_cast<const CancelOrder*>(cursor));
        break;
      case 'U':
        if (length >= sizeof(ReplaceOrder)) on_replace(*reinterpret_cast<const ReplaceOrder*>(cursor));
        break;
      default:
        break;
    }
    cursor += length;
  }
}

}  // namespace itch
