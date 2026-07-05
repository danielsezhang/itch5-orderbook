#pragma once

#include <cstddef>
#include <filesystem>

namespace itch {

// Read-only memory mapping of a feed file, so the parser reads messages straight out of the
// page cache with no intermediate buffer. Throws std::system_error naming the failed call.
class MappedFile {
 public:
  explicit MappedFile(const std::filesystem::path& path);
  ~MappedFile();
  MappedFile(const MappedFile&) = delete;
  MappedFile& operator=(const MappedFile&) = delete;
  MappedFile(MappedFile&& other) noexcept;
  MappedFile& operator=(MappedFile&& other) noexcept;

  [[nodiscard]] const std::byte* data() const noexcept { return data_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  void reset() noexcept;

  int fd_{-1};
  const std::byte* data_{nullptr};
  std::size_t size_{};
};

}  // namespace itch
