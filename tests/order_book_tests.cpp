#include "itch/order_book.hpp"

#include <catch2/catch_test_macros.hpp>

TEST_CASE("BBO follows add, execute, cancel, and replace mutations") {
  itch::OrderBook book(20'000);
  book.add(101, itch::Side::bid, 100, 10'000);
  book.add(102, itch::Side::bid, 50, 10'000);
  book.add(201, itch::Side::ask, 80, 10'010);

  REQUIRE(book.best_bid().price == 10'000);
  REQUIRE(book.best_bid().shares == 150);
  REQUIRE(book.best_ask().price == 10'010);

  book.execute(101, 40);
  REQUIRE(book.best_bid().shares == 110);

  book.cancel(102, 50);
  REQUIRE(book.best_bid().shares == 60);

  book.replace(101, 103, 70, 10'005);
  REQUIRE(book.best_bid().price == 10'005);
  REQUIRE(book.best_bid().shares == 70);

  book.execute(103, 70);
  REQUIRE_FALSE(book.best_bid().valid());
  REQUIRE(book.order_count() == 1);
}

TEST_CASE("price levels preserve FIFO order while partial cancels retain orders") {
  itch::OrderBook book(1'000);
  book.add(1, itch::Side::ask, 20, 300);
  book.add(2, itch::Side::ask, 30, 300);
  book.cancel(1, 10);

  REQUIRE(book.best_ask().price == 300);
  REQUIRE(book.best_ask().shares == 40);
  book.execute(1, 10);
  REQUIRE(book.best_ask().shares == 30);
  book.execute(2, 30);
  REQUIRE_FALSE(book.best_ask().valid());
}
