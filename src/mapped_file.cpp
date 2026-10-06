#include "itch/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <system_error>
#include <utility>

namespace itch {
namespace {

[[noreturn]] void fail(const char* operation, const std::filesystem::path& path) {
  throw std::system_error(errno, std::generic_category(),
                          std::string(operation) + " " + path.string());
}

}  // namespace

MappedFile::MappedFile(const std::filesystem::path& path) {
  fd_ = open(path.c_str(), O_RDONLY);
  if (fd_ == -1) fail("open", path);
  struct stat status{};
  if (fstat(fd_, &status) == -1) {
    reset();
    fail("fstat", path);
  }
  if (status.st_size == 0) {
    reset();
    throw std::runtime_error("empty ITCH file " + path.string());
  }
  size_ = static_cast<std::size_t>(status.st_size);
  void* mapping = mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0);
  if (mapping == MAP_FAILED) {
    reset();
    fail("mmap", path);
  }
  data_ = static_cast<const std::byte*>(mapping);
  // Hint the kernel to read ahead aggressively: the parser walks the file front to back once.
  madvise(mapping, size_, MADV_SEQUENTIAL);
}

MappedFile::~MappedFile() { reset(); }

void MappedFile::reset() noexcept {
  if (data_ != nullptr) munmap(const_cast<std::byte*>(data_), size_);
  if (fd_ != -1) close(fd_);
  fd_ = -1;
  data_ = nullptr;
  size_ = 0;
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    reset();
    fd_ = std::exchange(other.fd_, -1);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

}  // namespace itch
