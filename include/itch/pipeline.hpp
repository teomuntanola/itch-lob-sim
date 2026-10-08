#pragma once
// parser -> order book -> latency, for a single symbol.
//
// Only the book update itself is timed (two clock reads around it); CSV export
// happens after the stop timestamp so it never pollutes the latency numbers.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

#include "itch/latency.hpp"
#include "itch/order_book.hpp"
#include "itch/parser.hpp"

namespace itch {

struct PipelineOptions {
  std::string symbol;                              // e.g. "AAPL"
  std::string bbo_path;                            // empty = no BBO export
  std::string depth_path;                          // empty = no depth export
  size_t depth_levels = 5;
  uint64_t depth_interval_ns = 1'000'000'000ULL;   // market-time interval between depth snapshots
};

class Pipeline {
 public:
  Pipeline(const PipelineOptions& opt, OrderBook& book, LatencyRecorder& rec)
      : opt_(opt), book_(book), rec_(rec) {
    std::memset(sym_, ' ', sizeof(sym_));
    std::memcpy(sym_, opt.symbol.data(), std::min<size_t>(opt.symbol.size(), sizeof(sym_)));
    if (!opt.bbo_path.empty()) {
      bbo_ = std::fopen(opt.bbo_path.c_str(), "w");
      if (bbo_) {
        std::setvbuf(bbo_, nullptr, _IOFBF, 1 << 20);
        std::fputs("ts_ns,bid_px,bid_qty,ask_px,ask_qty\n", bbo_);
      }
    }
    if (!opt.depth_path.empty()) {
      depth_ = std::fopen(opt.depth_path.c_str(), "w");
      if (depth_) {
        std::setvbuf(depth_, nullptr, _IOFBF, 1 << 20);
        std::fputs("ts_ns,side,level,price,qty\n", depth_);
      }
    }
  }
  ~Pipeline() {
    if (bbo_) std::fclose(bbo_);
    if (depth_) std::fclose(depth_);
  }
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  void process_frame(const uint8_t* msg, size_t len) {
    ++total_;
    if (!parse_message(msg, len, *this)) ++unsupported_;
  }

  // ---- handlers called by parse_message ----
  void on(const StockDirectory& m) {
    if (!found_ && std::memcmp(m.stock, sym_, sizeof(sym_)) == 0) {
      found_ = true;
      locate_ = m.locate;
    }
  }
  void on(const AddOrder& m) {
    if (!mine(m.locate)) return;
    timed(m.ts, [&] { book_.add(m.ref, m.side, m.shares, m.price); });
  }
  void on(const OrderExecuted& m) {
    if (!mine(m.locate)) return;
    timed(m.ts, [&] { book_.reduce(m.ref, m.shares); });
  }
  void on(const OrderCancel& m) {
    if (!mine(m.locate)) return;
    timed(m.ts, [&] { book_.reduce(m.ref, m.shares); });
  }
  void on(const OrderDelete& m) {
    if (!mine(m.locate)) return;
    timed(m.ts, [&] { book_.remove(m.ref); });
  }
  void on(const OrderReplace& m) {
    if (!mine(m.locate)) return;
    timed(m.ts, [&] { book_.replace(m.old_ref, m.new_ref, m.shares, m.price); });
  }

  bool symbol_found() const { return found_; }
  uint16_t locate() const { return locate_; }
  uint64_t total_messages() const { return total_; }
  uint64_t symbol_messages() const { return processed_; }
  uint64_t unsupported_messages() const { return unsupported_; }

 private:
  bool mine(uint16_t locate) const { return found_ && locate == locate_; }

  template <class F>
  void timed(uint64_t ts, F&& f) {
    const uint64_t t0 = now_ns();
    f();
    const uint64_t t1 = now_ns();
    rec_.record(t1 - t0);
    ++processed_;
    export_after(ts);
  }

  static void put_px(std::FILE* f, uint32_t px) {
    std::fprintf(f, "%u.%04u", px / 10000u, px % 10000u);
  }
  static void put_level(std::FILE* f, const std::optional<Level>& l) {
    if (!l) {
      std::fputc(',', f);
      return;
    }
    put_px(f, l->price);
    std::fprintf(f, ",%llu", static_cast<unsigned long long>(l->qty));
  }

  void export_after(uint64_t ts) {
    if (bbo_) {
      const auto bb = book_.best_bid();
      const auto ba = book_.best_ask();
      if (bb != last_bb_ || ba != last_ba_) {
        std::fprintf(bbo_, "%llu,", static_cast<unsigned long long>(ts));
        put_level(bbo_, bb);
        std::fputc(',', bbo_);
        put_level(bbo_, ba);
        std::fputc('\n', bbo_);
        last_bb_ = bb;
        last_ba_ = ba;
      }
    }
    if (depth_ && ts >= next_depth_ts_) {
      for (int s = 0; s < 2; ++s) {
        const auto lv = book_.depth(s == 0 ? Side::Bid : Side::Ask, opt_.depth_levels);
        for (size_t i = 0; i < lv.size(); ++i) {
          std::fprintf(depth_, "%llu,%s,%zu,", static_cast<unsigned long long>(ts),
                       s == 0 ? "bid" : "ask", i + 1);
          put_px(depth_, lv[i].price);
          std::fprintf(depth_, ",%llu\n", static_cast<unsigned long long>(lv[i].qty));
        }
      }
      next_depth_ts_ = ts + opt_.depth_interval_ns;
    }
  }

  PipelineOptions opt_;
  OrderBook& book_;
  LatencyRecorder& rec_;
  char sym_[8];
  bool found_ = false;
  uint16_t locate_ = 0;
  uint64_t total_ = 0, processed_ = 0, unsupported_ = 0;
  std::FILE* bbo_ = nullptr;
  std::FILE* depth_ = nullptr;
  std::optional<Level> last_bb_, last_ba_;
  uint64_t next_depth_ts_ = 0;
};

}  // namespace itch
