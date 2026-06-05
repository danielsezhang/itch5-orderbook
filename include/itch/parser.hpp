#pragma once

#include "itch/messages.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

namespace itch {

class Parser {
 public:
  using AddHandler = std::function<void(const AddOrder&)>;
  using ExecuteHandler = std::function<void(const ExecuteOrder&)>;
  using CancelHandler = std::function<void(const CancelOrder&)>;
  using ReplaceHandler = std::function<void(const ReplaceOrder&)>;

  Parser(const std::byte* data, std::size_t size) : current_(data), end_(data + size) {}
  void parse(const AddHandler& on_add, const ExecuteHandler& on_execute,
             const CancelHandler& on_cancel, const ReplaceHandler& on_replace) const;

 private:
  const std::byte* current_;
  const std::byte* end_;
};

}  // namespace itch
