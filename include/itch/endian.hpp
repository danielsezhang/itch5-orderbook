#pragma once

#include <bit>
#include <cstdint>

namespace itch {

inline std::uint16_t be16(std::uint16_t value) noexcept {
  return std::endian::native == std::endian::big ? value : __builtin_bswap16(value);
}

inline std::uint32_t be32(std::uint32_t value) noexcept {
  return std::endian::native == std::endian::big ? value : __builtin_bswap32(value);
}

inline std::uint64_t be64(std::uint64_t value) noexcept {
  return std::endian::native == std::endian::big ? value : __builtin_bswap64(value);
}

inline std::uint64_t be48(const std::uint8_t (&bytes)[6]) noexcept {
  std::uint64_t value = 0;
  for (const auto byte : bytes) {
    value = (value << 8) | byte;
  }
  return value;
}

}  // namespace itch
