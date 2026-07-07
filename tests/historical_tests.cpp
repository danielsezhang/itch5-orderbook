// Replays a full day of real Nasdaq ITCH 5.0 data through MarketReplay and, in lockstep, through
// the naive ReferenceBook oracle. Requires the sample file (scripts/fetch_sample.sh); skipped
// when it is absent. Override the path with ITCH_SAMPLE.

#include "itch/mapped_file.hpp"
#include "itch/market_replay.hpp"
#include "itch/parser.hpp"
#include "itch/sharded_replay.hpp"
#include "book_compare.hpp"
#include "reference_book.hpp"

#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

using itch::BestQuote;
using itch::Side;
using itch::Status;
using itch::testing::ReferenceBook;

std::uint64_t anomalies(const itch::ReplayStats& stats) { return stats.anomalies(); }

bool same(const BestQuote& a, const BestQuote& b) {
  return a.shares == b.shares && (!a.valid() || a.price == b.price);
}

class DifferentialReplay {
 public:
  explicit DifferentialReplay(itch::OrderPool& pool) : replay_(pool), refs_(UINT16_MAX + 1) {}

  void on(const itch::AddOrder& m) {
    const Side side = m.side == 'B' ? Side::bid : Side::ask;
    const auto before = anomalies(replay_.stats());
    replay_.on(m);
    check(m.header.locate(), before,
          ref(m.header.locate()).add(m.order_reference(), side, m.share_count(), m.price_ticks()));
  }
  void on(const itch::ExecuteOrder& m) {
    observe_execution(m.header.locate(), m.order_reference());
    const auto before = anomalies(replay_.stats());
    replay_.on(m);
    check(m.header.locate(), before, ref(m.header.locate()).reduce(m.order_reference(), m.share_count()));
  }
  void on(const itch::ExecuteWithPrice& m) {
    const auto before = anomalies(replay_.stats());
    replay_.on(m);
    check(m.header.locate(), before, ref(m.header.locate()).reduce(m.order_reference(), m.share_count()));
  }
  void on(const itch::CancelOrder& m) {
    const auto before = anomalies(replay_.stats());
    replay_.on(m);
    check(m.header.locate(), before, ref(m.header.locate()).reduce(m.order_reference(), m.share_count()));
  }
  void on(const itch::DeleteOrder& m) {
    const auto before = anomalies(replay_.stats());
    replay_.on(m);
    check(m.header.locate(), before, ref(m.header.locate()).remove(m.order_reference()));
  }
  void on(const itch::ReplaceOrder& m) {
    const auto before = anomalies(replay_.stats());
    replay_.on(m);
    check(m.header.locate(), before,
          ref(m.header.locate())
              .replace(m.original_order_reference(), m.replacement_order_reference(),
                       m.share_count(), m.price_ticks()));
  }

  // Full Level 3 comparison of every book: levels, FIFO order, references, open shares.
  std::uint64_t full_diff() {
    std::uint64_t books_with_differences = 0;
    for (std::size_t locate = 0; locate < refs_.size(); ++locate) {
      if (!refs_[locate]) continue;
      const itch::OrderBook* book = replay_.book(static_cast<std::uint16_t>(locate));
      const std::string diff = book ? refs_[locate]->diff(*book) : std::string("missing book");
      if (!diff.empty()) {
        if (++books_with_differences <= 5) UNSCOPED_INFO("locate " << locate << ": " << diff);
      }
    }
    ++full_diffs;
    return books_with_differences;
  }

  const itch::MarketReplay& replay() const { return replay_; }

  std::uint64_t messages{};
  std::uint64_t status_mismatches{};
  std::uint64_t bbo_mismatches{};
  std::uint64_t full_diffs{};
  std::uint64_t full_diff_failures{};
  std::uint64_t executions{};
  std::uint64_t executions_at_touch{};
  std::uint64_t executions_at_queue_head{};

 private:
  ReferenceBook& ref(std::uint16_t locate) {
    auto& book = refs_[locate];
    if (!book) book = std::make_unique<ReferenceBook>();
    return *book;
  }

  // Real executions should hit the best price and the front of the queue; measuring how often
  // they do is an independent check that price-time priority was reconstructed correctly.
  void observe_execution(std::uint16_t locate, std::uint64_t reference) {
    const itch::OrderBook* book = replay_.book(locate);
    const itch::Order* order = book ? book->find(reference) : nullptr;
    if (!order) return;
    ++executions;
    const BestQuote best = order->side == Side::bid ? book->best_bid() : book->best_ask();
    if (best.price == order->price) {
      ++executions_at_touch;
      if (order->prev == nullptr) ++executions_at_queue_head;
    }
  }

  void check(std::uint16_t locate, std::uint64_t anomalies_before, Status expected) {
    const bool fast_ok = anomalies(replay_.stats()) == anomalies_before;
    if (fast_ok != (expected == Status::ok)) ++status_mismatches;
    const itch::OrderBook* book = replay_.book(locate);
    const ReferenceBook& oracle = *refs_[locate];
    if (!same(book->best_bid(), oracle.best(Side::bid)) ||
        !same(book->best_ask(), oracle.best(Side::ask))) {
      if (++bbo_mismatches <= 5) UNSCOPED_INFO("BBO mismatch at message " << messages << " locate " << locate);
    }
    if (++messages % 10'000'000 == 0 && full_diff() != 0) ++full_diff_failures;
  }

  itch::MarketReplay replay_;
  std::vector<std::unique_ptr<ReferenceBook>> refs_;
};

std::filesystem::path sample_path() {
  if (const char* env = std::getenv("ITCH_SAMPLE")) return env;
  return ITCH_DEFAULT_SAMPLE;
}

#define REQUIRE_SAMPLE(path)                                                    \
  if (!std::filesystem::exists(path)) SKIP("historical sample not found: " << (path).string())

}  // namespace

TEST_CASE("full-day Nasdaq replay matches the reference book at every message", "[historical]") {
  const auto path = sample_path();
  REQUIRE_SAMPLE(path);

  const itch::MappedFile file(path);
  itch::OrderPool pool;
  auto differential = std::make_unique<DifferentialReplay>(pool);
  const std::uint64_t parsed = itch::Parser(file.data(), file.size()).parse(*differential);
  const std::uint64_t final_differences = differential->full_diff();

  const auto& d = *differential;
  const auto& s = d.replay().stats();
  std::printf(
      "historical: %llu messages, %llu order messages, %zu books, %zu open orders\n"
      "  adds=%llu E=%llu C=%llu X=%llu D=%llu U=%llu\n"
      "  status mismatches=%llu BBO mismatches=%llu full L3 diffs=%llu (failures %llu, final %llu)\n"
      "  executions at touch %.4f%%, at touch and queue head %.4f%%\n",
      static_cast<unsigned long long>(parsed), static_cast<unsigned long long>(d.messages),
      d.replay().book_count(), d.replay().open_orders(),
      static_cast<unsigned long long>(s.adds), static_cast<unsigned long long>(s.executes),
      static_cast<unsigned long long>(s.executes_with_price), static_cast<unsigned long long>(s.cancels),
      static_cast<unsigned long long>(s.deletes), static_cast<unsigned long long>(s.replaces),
      static_cast<unsigned long long>(d.status_mismatches), static_cast<unsigned long long>(d.bbo_mismatches),
      static_cast<unsigned long long>(d.full_diffs), static_cast<unsigned long long>(d.full_diff_failures),
      static_cast<unsigned long long>(final_differences),
      100.0 * static_cast<double>(d.executions_at_touch) / static_cast<double>(d.executions),
      100.0 * static_cast<double>(d.executions_at_queue_head) / static_cast<double>(d.executions));

  REQUIRE(d.messages > 1'000'000);
  REQUIRE(anomalies(s) == 0);
  REQUIRE(d.status_mismatches == 0);
  REQUIRE(d.bbo_mismatches == 0);
  REQUIRE(d.full_diff_failures == 0);
  REQUIRE(final_differences == 0);
  REQUIRE(d.replay().open_orders() == [&] {
    std::size_t total = 0;
    for (std::uint32_t locate = 0; locate <= UINT16_MAX; ++locate) {
      if (const auto* book = d.replay().book(static_cast<std::uint16_t>(locate))) total += book->order_count();
    }
    return total;
  }());
}

TEST_CASE("sharded replay matches the sequential replay book for book mid-session",
          "[historical]") {
  const auto path = sample_path();
  REQUIRE_SAMPLE(path);
  const itch::MappedFile file(path);

  // Every book is empty after the close, so compare at cut points where the books are full.
  std::vector<std::size_t> cuts;
  {
    const std::uint64_t total = itch::Parser(file.data(), file.size()).for_each_message([](auto) {});
    std::uint64_t index = 0;
    itch::Parser(file.data(), file.size()).for_each_message([&](const std::byte* message) {
      if (++index == total / 4 || index == total / 2 || index == total * 3 / 4) {
        cuts.push_back(static_cast<std::size_t>(message - file.data()) - 2);  // before its prefix
      }
    });
  }
  REQUIRE(cuts.size() == 3);

  for (const std::size_t cut : cuts) {
    itch::OrderPool pool;
    itch::MarketReplay sequential(pool);
    const std::uint64_t parsed = itch::Parser(file.data(), cut).parse(sequential);
    REQUIRE(sequential.open_orders() > 10'000);

    for (const unsigned shards : {2u, 4u, 8u}) {
      INFO("cut at byte " << cut << ", " << shards << " shards");
      itch::ShardedReplay sharded(pool, shards);
      REQUIRE(sharded.replay(file.data(), cut) == parsed);
      REQUIRE(sharded.stats() == sequential.stats());
      REQUIRE(sharded.open_orders() == sequential.open_orders());

      std::uint64_t books_with_differences = 0;
      for (std::uint32_t locate = 0; locate <= UINT16_MAX; ++locate) {
        const auto* expected = sequential.book(static_cast<std::uint16_t>(locate));
        const auto* actual = sharded.book(static_cast<std::uint16_t>(locate));
        if ((expected == nullptr) != (actual == nullptr) ||
            (expected != nullptr && !itch::testing::diff_books(*expected, *actual).empty())) {
          ++books_with_differences;
        }
      }
      REQUIRE(books_with_differences == 0);
    }
    std::printf("mid-session cut: %llu messages, %zu open orders, sharded == sequential\n",
                static_cast<unsigned long long>(parsed), sequential.open_orders());
  }
}
