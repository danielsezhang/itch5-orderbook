#include "itch/mapped_file.hpp"

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <stdexcept>
#include <utility>

namespace itch {

MappedFile::MappedFile(const std::filesystem::path& path) {
  fd_ = open(path.c_str(), O_RDONLY);
  if (fd_ == -1) {
    throw std::runtime_error("unable to open ITCH file");
  }
  struct stat status {};
  if (fstat(fd_, &status) == -1 || status.st_size == 0) {
    close(fd_);
    throw std::runtime_error("unable to stat non-empty ITCH file");
  }
  size_ = static_cast<std::size_t>(status.st_size);
  data_ = static_cast<const std::byte*>(mmap(nullptr, size_, PROT_READ, MAP_PRIVATE, fd_, 0));
  if (data_ == MAP_FAILED) {
    close(fd_);
    data_ = nullptr;
    throw std::runtime_error("unable to mmap ITCH file");
  }
}

MappedFile::~MappedFile() {
  if (data_ != nullptr) munmap(const_cast<std::byte*>(data_), size_);
  if (fd_ != -1) close(fd_);
}

MappedFile::MappedFile(MappedFile&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      data_(std::exchange(other.data_, nullptr)),
      size_(std::exchange(other.size_, 0)) {}

MappedFile& MappedFile::operator=(MappedFile&& other) noexcept {
  if (this != &other) {
    this->~MappedFile();
    fd_ = std::exchange(other.fd_, -1);
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
  }
  return *this;
}

}  // namespace itch
