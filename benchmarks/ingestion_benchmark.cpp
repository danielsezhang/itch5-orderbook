#include "itch/order_book.hpp"

#include <benchmark/benchmark.h>

static void BM_OrderMutations(benchmark::State& state) {
  for (auto _ : state) {
    itch::OrderBook book(100'000);
    for (std::uint64_t reference = 1; reference <= 1'000; ++reference) {
      book.add(reference, reference % 2 ? itch::Side::bid : itch::Side::ask, 100,
               50'000 + static_cast<std::uint32_t>(reference % 100));
    }
    benchmark::DoNotOptimize(book.best_bid().price);
  }
}
BENCHMARK(BM_OrderMutations);
