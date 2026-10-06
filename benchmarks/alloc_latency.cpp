// Allocation latency of OrderPool. Apple Silicon timers tick every 41.67 ns, too coarse for a
// single allocation, so each sample times a batch of kOpsPerSample consecutive allocate() calls
// (timer overhead included, not subtracted) and reports per-allocation latency for that batch.
// Usage: itch_alloc_latency [threads...]   default: 1 2 4 8
#include "itch/order_pool.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

constexpr int kOpsPerSample = 16;
constexpr int kSamplesPerThread = 2'000'000;
// Each thread keeps up to this many slots live, so the cache overflows and refills through the
// shared Treiber stack every 64 operations rather than recycling purely thread-locally.
constexpr int kLiveWindow = 512;

std::uint64_t now_ns() {
  return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                        std::chrono::steady_clock::now().time_since_epoch())
                                        .count());
}

double percentile(const std::vector<double>& sorted, double p) {
  return sorted[std::min(sorted.size() - 1, static_cast<std::size_t>(p * sorted.size()))];
}

void run(int threads) {
  itch::OrderPool pool(std::size_t{1} << 22);
  std::vector<std::vector<double>> samples(threads);
  std::atomic<int> ready{0};
  std::vector<std::thread> workers;
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      auto& out = samples[t];
      out.reserve(kSamplesPerThread);
      std::vector<itch::Order*> live;
      live.reserve(kLiveWindow + kOpsPerSample);
      ready.fetch_add(1);
      while (ready.load() < threads) {}
      for (int sample = 0; sample < kSamplesPerThread; ++sample) {
        itch::Order* batch[kOpsPerSample];
        const std::uint64_t start = now_ns();
        for (auto& slot : batch) slot = pool.allocate();
        const std::uint64_t stop = now_ns();
        out.push_back(static_cast<double>(stop - start) / kOpsPerSample);
        for (auto* slot : batch) {
          slot->reference = static_cast<std::uint64_t>(sample);  // touch like a real order
          live.push_back(slot);
        }
        if (live.size() >= kLiveWindow) {
          for (auto* slot : live) pool.release(slot);
          live.clear();
        }
      }
      for (auto* slot : live) pool.release(slot);
      itch::OrderPool::flush_thread_cache();
    });
  }
  for (auto& worker : workers) worker.join();

  std::vector<double> all;
  for (auto& part : samples) all.insert(all.end(), part.begin(), part.end());
  const std::size_t warmup = all.size() / 100;
  std::sort(all.begin() + static_cast<std::ptrdiff_t>(warmup), all.end());
  std::vector<double> sorted(all.begin() + static_cast<std::ptrdiff_t>(warmup), all.end());
  std::printf("threads=%d samples=%zu  p50=%.1f ns  p99=%.1f ns  p99.9=%.1f ns  max=%.1f ns\n",
              threads, sorted.size(), percentile(sorted, 0.50), percentile(sorted, 0.99),
              percentile(sorted, 0.999), sorted.back());
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<double> overhead;
  for (int i = 0; i < 1'000'000; ++i) {
    const auto a = now_ns();
    const auto b = now_ns();
    overhead.push_back(static_cast<double>(b - a));
  }
  std::sort(overhead.begin(), overhead.end());
  std::printf("cas128_lock_free=%d  ops_per_sample=%d  timer_pair_p50=%.1f ns (not subtracted)\n",
              itch::OrderPool::cas128_is_lock_free(), kOpsPerSample, percentile(overhead, 0.5));
  std::vector<int> thread_counts;
  for (int i = 1; i < argc; ++i) thread_counts.push_back(std::atoi(argv[i]));
  if (thread_counts.empty()) thread_counts = {1, 2, 4, 8};
  for (int threads : thread_counts) run(threads);
  return 0;
}
