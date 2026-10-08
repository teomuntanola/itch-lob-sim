#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

namespace itch {

uint64_t now_ns();             // monotonic clock, nanoseconds
uint64_t timer_overhead_ns();  // median cost of a back-to-back now_ns() pair

struct LatencySummary {
  uint64_t count = 0;
  double mean = 0;
  uint32_t min = 0, p50 = 0, p90 = 0, p99 = 0, p999 = 0, p9999 = 0, max = 0;
};

// Stores every sample (4 bytes each) and computes exact percentiles at the end.
class LatencyRecorder {
 public:
  explicit LatencyRecorder(size_t reserve = 0) { samples_.reserve(reserve); }

  void record(uint64_t ns) {
    samples_.push_back(ns > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<uint32_t>(ns));
  }
  size_t count() const { return samples_.size(); }

  // Sorts the samples in place; call once, after the run.
  LatencySummary summarize();

 private:
  std::vector<uint32_t> samples_;
};

}  // namespace itch
