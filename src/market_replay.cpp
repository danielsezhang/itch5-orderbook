#include "itch/market_replay.hpp"

namespace itch {

ReplayStats& ReplayStats::operator+=(const ReplayStats& other) noexcept {
  adds += other.adds;
  executes += other.executes;
  executes_with_price += other.executes_with_price;
  cancels += other.cancels;
  deletes += other.deletes;
  replaces += other.replaces;
  unknown_reference += other.unknown_reference;
  duplicate_reference += other.duplicate_reference;
  exceeds_open_shares += other.exceeds_open_shares;
  invalid += other.invalid;
  return *this;
}

MarketReplay::MarketReplay(OrderPool& pool, std::uint32_t window_levels)
    : pool_(pool), window_levels_(window_levels), books_(UINT16_MAX + 1) {}

OrderBook& MarketReplay::book_for(std::uint16_t locate) {
  auto& book = books_[locate];
  if (!book) [[unlikely]] book = std::make_unique<OrderBook>(pool_, window_levels_);
  return *book;
}

void MarketReplay::record(Status status) noexcept {
  switch (status) {
    case Status::ok: break;
    case Status::unknown_reference: ++stats_.unknown_reference; break;
    case Status::duplicate_reference: ++stats_.duplicate_reference; break;
    case Status::exceeds_open_shares: ++stats_.exceeds_open_shares; break;
    case Status::invalid: ++stats_.invalid; break;
  }
}

void MarketReplay::on(const AddOrder& message) {
  ++stats_.adds;
  if (message.side != 'B' && message.side != 'S') {
    record(Status::invalid);
    return;
  }
  const Side side = message.side == 'B' ? Side::bid : Side::ask;
  record(book_for(message.header.locate())
             .add(message.order_reference(), side, message.share_count(), message.price_ticks()));
}

void MarketReplay::on(const ExecuteOrder& message) {
  ++stats_.executes;
  record(book_for(message.header.locate())
             .execute(message.order_reference(), message.share_count()));
}

void MarketReplay::on(const ExecuteWithPrice& message) {
  ++stats_.executes_with_price;
  record(book_for(message.header.locate())
             .execute(message.order_reference(), message.share_count()));
}

void MarketReplay::on(const CancelOrder& message) {
  ++stats_.cancels;
  record(book_for(message.header.locate())
             .cancel(message.order_reference(), message.share_count()));
}

void MarketReplay::on(const DeleteOrder& message) {
  ++stats_.deletes;
  record(book_for(message.header.locate()).remove(message.order_reference()));
}

void MarketReplay::on(const ReplaceOrder& message) {
  ++stats_.replaces;
  record(book_for(message.header.locate())
             .replace(message.original_order_reference(), message.replacement_order_reference(),
                      message.share_count(), message.price_ticks()));
}

std::size_t MarketReplay::book_count() const noexcept {
  std::size_t count = 0;
  for (const auto& book : books_) count += book != nullptr;
  return count;
}

std::size_t MarketReplay::open_orders() const noexcept {
  std::size_t count = 0;
  for (const auto& book : books_) {
    if (book) count += book->order_count();
  }
  return count;
}

}  // namespace itch
