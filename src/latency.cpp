#include "itch/latency.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>

namespace itch {

uint64_t now_ns() {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

uint64_t timer_overhead_ns() {
  constexpr int kN = 100000;
  std::vector<uint32_t> d(kN);
  for (int i = 0; i < kN; ++i) {
    const uint64_t a = now_ns();
    const uint64_t b = now_ns();
    d[i] = static_cast<uint32_t>(b - a);
  }
  std::nth_element(d.begin(), d.begin() + kN / 2, d.end());
  return d[kN / 2];
}

LatencySummary LatencyRecorder::summarize() {
  LatencySummary s;
  s.count = samples_.size();
  if (samples_.empty()) return s;
  std::sort(samples_.begin(), samples_.end());
  const size_t n = samples_.size();
  auto pct = [&](double p) {
    // nearest-rank method
    size_t rank = static_cast<size_t>(std::ceil(p / 100.0 * static_cast<double>(n)));
    if (rank < 1) rank = 1;
    if (rank > n) rank = n;
    return samples_[rank - 1];
  };
  double sum = 0;
  for (uint32_t v : samples_) sum += v;
  s.mean = sum / static_cast<double>(n);
  s.min = samples_.front();
  s.max = samples_.back();
  s.p50 = pct(50);
  s.p90 = pct(90);
  s.p99 = pct(99);
  s.p999 = pct(99.9);
  s.p9999 = pct(99.99);
  return s;
}

}  // namespace itch
