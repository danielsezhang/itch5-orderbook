// Throughput of each stage on a real Nasdaq BinaryFILE ITCH 5.0 capture:
//   1. parse only            every field of every order message decoded, nothing stored
//   2. sequential L3 books   parse and apply on one thread
//   3. sharded L3 books      parse on this thread, apply on N shard threads
// The file is read once beforehand so every run starts from the page cache. Each figure is the
// median of `runs` runs. Usage: itch_ingest <file> [runs] [shard counts...]
#include "itch/mapped_file.hpp"
#include "itch/market_replay.hpp"
#include "itch/parser.hpp"
#include "itch/sharded_replay.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include <sys/resource.h>

namespace {

// Folds every decoded field into a checksum so the compiler cannot skip the byte swaps.
struct DecodeOnly {
  std::uint64_t checksum{};
  void on(const itch::AddOrder& m) {
    checksum += m.order_reference() + m.share_count() + m.price_ticks();
  }
  void on(const itch::ExecuteOrder& m) { checksum += m.order_reference() + m.share_count(); }
  void on(const itch::ExecuteWithPrice& m) { checksum += m.order_reference() + m.price_ticks(); }
  void on(const itch::CancelOrder& m) { checksum += m.order_reference() + m.share_count(); }
  void on(const itch::DeleteOrder& m) { checksum += m.order_reference(); }
  void on(const itch::ReplaceOrder& m) {
    checksum += m.replacement_order_reference() + m.price_ticks();
  }
};

template <typename Work>
double seconds(Work&& work) {
  const auto start = std::chrono::steady_clock::now();
  work();
  return std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
}

// `run` performs one complete run and returns the seconds it timed, so setup and teardown
// (pool mapping, book destruction) stay outside the measurement.
template <typename Run>
double median_seconds(int runs, Run&& run) {
  std::vector<double> times;
  for (int i = 0; i < runs; ++i) times.push_back(run());
  std::sort(times.begin(), times.end());
  return times[times.size() / 2];
}

void report(const char* stage, double seconds, std::uint64_t messages, double baseline = 0) {
  std::printf("%-22s %7.3f s  %6.1f M msg/s  %6.1f ns/msg", stage, seconds,
              static_cast<double>(messages) / seconds / 1e6,
              seconds * 1e9 / static_cast<double>(messages));
  if (baseline > 0) std::printf("  %4.2fx", baseline / seconds);
  std::printf("\n");
}

double flat_array_share(const itch::MarketReplay& replay) {
  std::uint64_t window = 0;
  std::uint64_t overflow = 0;
  for (std::uint32_t locate = 0; locate <= UINT16_MAX; ++locate) {
    if (const auto* book = replay.book(static_cast<std::uint16_t>(locate))) {
      for (const auto side : {itch::Side::bid, itch::Side::ask}) {
        window += book->stats(side).window_inserts;
        overflow += book->stats(side).overflow_inserts;
      }
    }
  }
  return 100.0 * static_cast<double>(window) / static_cast<double>(window + overflow);
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <itch-file> [runs] [shard counts...]\n", argv[0]);
    return 2;
  }
  const int runs = argc > 2 ? std::max(1, std::atoi(argv[2])) : 5;
  std::vector<unsigned> shard_counts;
  for (int i = 3; i < argc; ++i) shard_counts.push_back(static_cast<unsigned>(std::atoi(argv[i])));
  if (shard_counts.empty()) shard_counts = {1, 2, 4, 6, 8};

  const itch::MappedFile file(argv[1]);
  const itch::Parser parser(file.data(), file.size());
  DecodeOnly warm_up;
  const std::uint64_t messages = parser.parse(warm_up);

  DecodeOnly decoder;
  const double parse_s =
      median_seconds(runs, [&] { return seconds([&] { parser.parse(decoder); }); });

  itch::ReplayStats stats{};
  double flat_share = 0;
  const double sequential_s = median_seconds(runs, [&] {
    itch::OrderPool pool;
    itch::MarketReplay replay(pool);
    const double elapsed = seconds([&] { parser.parse(replay); });
    stats = replay.stats();
    flat_share = flat_array_share(replay);
    return elapsed;
  });

  std::printf("file          %.1f MB, %llu messages, %llu order messages\n", file.size() / 1e6,
              static_cast<unsigned long long>(messages),
              static_cast<unsigned long long>(stats.order_messages()));
  std::printf("anomalies     %llu (unknown, duplicate, over-executed or invalid)\n",
              static_cast<unsigned long long>(stats.anomalies()));
  std::printf("flat array    %.4f%% of adds\n", flat_share);
  std::printf("runs          %d, median reported\n\n", runs);
  report("parse only", parse_s, messages);
  report("sequential L3", sequential_s, messages);

  for (const unsigned shards : shard_counts) {
    std::vector<std::uint64_t> routed(shards);
    const double sharded_s = median_seconds(runs, [&] {
      itch::OrderPool pool;
      itch::ShardedReplay replay(pool, shards);
      const double elapsed = seconds([&] { replay.replay(file.data(), file.size()); });
      for (unsigned i = 0; i < shards; ++i) routed[i] = replay.routed(i);
      return elapsed;
    });
    char stage[32];
    std::snprintf(stage, sizeof(stage), "sharded L3, %u shards", shards);
    report(stage, sharded_s, messages, sequential_s);
    const auto [lightest, busiest] = std::minmax_element(routed.begin(), routed.end());
    std::printf(
        "%-22s busiest shard %.1f%% of order messages, lightest %.1f%%\n", "",
        100.0 * static_cast<double>(*busiest) / static_cast<double>(stats.order_messages()),
        100.0 * static_cast<double>(*lightest) / static_cast<double>(stats.order_messages()));
  }

  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::printf("\nmax RSS       %.0f MB\n",
              static_cast<double>(usage.ru_maxrss) / 1048576.0);  // bytes on macOS
  std::printf("checksum      %llu\n", static_cast<unsigned long long>(decoder.checksum));
  return 0;
}
