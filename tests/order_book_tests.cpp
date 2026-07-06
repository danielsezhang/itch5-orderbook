#include "itch/order_book.hpp"
#include "reference_book.hpp"

#include <catch2/catch_test_macros.hpp>

#include <random>
#include <vector>

using itch::OrderBook;
using itch::Side;
using itch::Status;

namespace {
constexpr std::uint32_t dollars(double value) { return static_cast<std::uint32_t>(value * 10'000 + 0.5); }

std::vector<std::uint64_t> queue(const OrderBook& book, Side side, std::size_t level_index) {
  std::vector<std::uint64_t> references;
  const auto levels = book.depth(side);
  for (const auto* order = levels.at(level_index).level->head; order; order = order->next) {
    references.push_back(order->reference);
  }
  return references;
}
}  // namespace

TEST_CASE("BBO follows add, execute, cancel, delete, and replace") {
  itch::OrderPool pool(1 << 16);
  OrderBook book(pool);
  REQUIRE(book.add(101, Side::bid, 100, dollars(10.00)) == Status::ok);
  REQUIRE(book.add(102, Side::bid, 50, dollars(10.00)) == Status::ok);
  REQUIRE(book.add(201, Side::ask, 80, dollars(10.01)) == Status::ok);

  REQUIRE(book.best_bid().price == dollars(10.00));
  REQUIRE(book.best_bid().shares == 150);
  REQUIRE(book.best_ask().price == dollars(10.01));

  REQUIRE(book.execute(101, 40) == Status::ok);
  REQUIRE(book.best_bid().shares == 110);
  REQUIRE(book.cancel(102, 50) == Status::ok);
  REQUIRE(book.best_bid().shares == 60);

  REQUIRE(book.replace(101, 103, 70, dollars(9.99)) == Status::ok);
  REQUIRE(book.find(101) == nullptr);
  REQUIRE(book.find(103)->side == Side::bid);

  REQUIRE(book.remove(103) == Status::ok);
  REQUIRE_FALSE(book.best_bid().valid());
  REQUIRE(book.remove(103) == Status::unknown_reference);
  REQUIRE(book.order_count() == 1);
}

TEST_CASE("levels keep FIFO priority and replace loses time priority") {
  itch::OrderPool pool(1 << 16);
  OrderBook book(pool);
  book.add(1, Side::ask, 20, dollars(30));
  book.add(2, Side::ask, 30, dollars(30));
  book.add(3, Side::ask, 40, dollars(30));
  REQUIRE(queue(book, Side::ask, 0) == std::vector<std::uint64_t>{1, 2, 3});

  REQUIRE(book.cancel(2, 10) == Status::ok);  // partial cancel keeps position
  REQUIRE(queue(book, Side::ask, 0) == std::vector<std::uint64_t>{1, 2, 3});

  REQUIRE(book.replace(1, 4, 20, dollars(30)) == Status::ok);  // same price, back of queue
  REQUIRE(queue(book, Side::ask, 0) == std::vector<std::uint64_t>{2, 3, 4});
  REQUIRE(book.best_ask().shares == 20 + 40 + 20);
}

TEST_CASE("invalid and inconsistent messages are reported, not applied silently") {
  itch::OrderPool pool(1 << 16);
  OrderBook book(pool);
  REQUIRE(book.add(1, Side::bid, 0, dollars(5)) == Status::invalid);
  REQUIRE(book.add(1, Side::bid, 10, dollars(5)) == Status::ok);
  REQUIRE(book.add(1, Side::ask, 10, dollars(6)) == Status::duplicate_reference);
  REQUIRE(book.execute(9, 1) == Status::unknown_reference);
  REQUIRE(book.add(2, Side::bid, 10, dollars(4)) == Status::ok);
  REQUIRE(book.replace(1, 2, 5, dollars(5)) == Status::duplicate_reference);
  REQUIRE(book.find(1) != nullptr);
  REQUIRE(book.execute(1, 11) == Status::exceeds_open_shares);
  REQUIRE(book.find(1) == nullptr);
}

TEST_CASE("best price is found across bitmap word boundaries and outside the window") {
  itch::OrderPool pool(1 << 16);
  OrderBook book(pool, 256);
  // 256 cent-levels per side. Spread orders over several 64-bit words, then far outside.
  book.add(1, Side::bid, 10, dollars(50.00));
  book.add(2, Side::bid, 10, dollars(49.30));
  book.add(3, Side::bid, 10, dollars(48.70));
  book.add(4, Side::bid, 10, dollars(1.00));  // overflow: far below the window
  REQUIRE(book.best_bid().price == dollars(50.00));
  book.remove(1);
  REQUIRE(book.best_bid().price == dollars(49.30));
  book.remove(2);
  REQUIRE(book.best_bid().price == dollars(48.70));
  book.remove(3);
  REQUIRE(book.best_bid().price == dollars(1.00));
  REQUIRE(book.stats(Side::bid).overflow_inserts == 1);

  book.add(5, Side::ask, 10, dollars(0.0123));  // sub-dollar ticks are $0.0001
  book.add(6, Side::ask, 10, dollars(0.0122));
  REQUIRE(book.best_ask().price == dollars(0.0122));
}

TEST_CASE("window re-centres when the touch moves away and keeps one level per price") {
  itch::OrderPool pool(1 << 16);
  itch::testing::ReferenceBook reference;
  OrderBook book(pool, 64);
  std::uint64_t id = 1;
  auto add = [&](Side side, std::uint32_t price) {
    REQUIRE(book.add(id, side, 100, price) == reference.add(id, side, 100, price));
    ++id;
  };
  add(Side::bid, dollars(0.01));    // stub quote anchors the first window
  add(Side::bid, dollars(20.00));   // new best far away: window must follow the touch
  add(Side::bid, dollars(19.99));
  REQUIRE(book.stats(Side::bid).recenters == 2);
  REQUIRE(book.stats(Side::bid).window_inserts == 3);
  add(Side::bid, dollars(25.00));   // moves up again, 19.99/20.00 migrate to overflow
  add(Side::bid, dollars(20.00));   // joins the existing 20.00 queue, wherever it lives
  REQUIRE(reference.diff(book).empty());
  REQUIRE(book.depth(Side::bid).size() == 4);
  REQUIRE(queue(book, Side::bid, 1) == std::vector<std::uint64_t>{2, 5});
}

TEST_CASE("off-grid prices at or above $1 are kept in exact price order") {
  itch::OrderPool pool(1 << 16);
  itch::testing::ReferenceBook reference;
  OrderBook book(pool, 64);
  for (const auto& [id, price] : {std::pair{1u, dollars(10.00)}, {2u, dollars(10.00) + 50},
                                 {3u, dollars(10.01)}, {4u, dollars(9.99) + 1}}) {
    REQUIRE(book.add(id, Side::bid, 10, price) == reference.add(id, Side::bid, 10, price));
  }
  REQUIRE(book.best_bid().price == dollars(10.01));
  REQUIRE(reference.diff(book).empty());
}

TEST_CASE("randomised differential test against a std::map reference book") {
  std::mt19937_64 rng(20191230);
  for (const std::uint32_t window : {64u, 1024u}) {
    itch::OrderPool pool(1 << 20);
    OrderBook book(pool, window);
    itch::testing::ReferenceBook reference;
    std::vector<std::uint64_t> live;
    std::uint64_t next_id = 1;
    std::uint32_t mid = dollars(50);
    for (int step = 0; step < 200'000; ++step) {
      if (step % 5'000 == 0) mid = dollars(20) + static_cast<std::uint32_t>(rng() % 6'000) * 100;
      const int op = static_cast<int>(rng() % 100);
      const auto pick = [&] { return live[rng() % live.size()]; };
      Status fast;
      Status expected;
      if (op < 40 || live.empty()) {
        const Side side = rng() % 2 ? Side::bid : Side::ask;
        std::uint32_t price;
        if (rng() % 50 == 0) {
          price = 1 + static_cast<std::uint32_t>(rng() % dollars(500));  // stubs, sub-penny
        } else {
          const auto offset = static_cast<std::uint32_t>(rng() % 300) * 100;
          price = side == Side::bid ? mid - offset : mid + 100 + offset;
        }
        const auto shares = 1 + static_cast<std::uint32_t>(rng() % 1'000);
        fast = book.add(next_id, side, shares, price);
        expected = reference.add(next_id, side, shares, price);
        live.push_back(next_id++);
      } else if (op < 60) {
        const auto id = pick();
        const auto shares = 1 + static_cast<std::uint32_t>(rng() % 600);
        fast = book.execute(id, shares);
        expected = reference.reduce(id, shares);
      } else if (op < 75) {
        const auto id = pick();
        const auto shares = 1 + static_cast<std::uint32_t>(rng() % 600);
        fast = book.cancel(id, shares);
        expected = reference.reduce(id, shares);
      } else if (op < 90) {
        const auto id = pick();
        fast = book.remove(id);
        expected = reference.remove(id);
      } else {
        const auto id = pick();
        const itch::Order* order = book.find(id);
        const std::uint32_t base = order ? order->price : mid;
        const std::uint32_t price = base + static_cast<std::uint32_t>(rng() % 5) * 100;
        fast = book.replace(id, next_id, 100, price);
        expected = reference.replace(id, next_id, 100, price);
        live.push_back(next_id++);
      }
      REQUIRE(fast == expected);
      REQUIRE(book.best_bid().price == reference.best(Side::bid).price);
      REQUIRE(book.best_bid().shares == reference.best(Side::bid).shares);
      REQUIRE(book.best_ask().price == reference.best(Side::ask).price);
      REQUIRE(book.best_ask().shares == reference.best(Side::ask).shares);
      if (step % 10'000 == 0) REQUIRE(reference.diff(book).empty());
      if (live.size() > 20'000) live.erase(live.begin(), live.begin() + 10'000);
    }
    REQUIRE(reference.diff(book).empty());
  }
}
