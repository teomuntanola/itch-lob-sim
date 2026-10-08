#pragma once
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>

#include "itch/types.hpp"

namespace itch {

// L2 (price-aggregated) order book fed by L3 (per-order) ITCH events.
//
// Layout: each side keeps a dense ladder (one slot per tick) over a price
// window of `window` ticks, plus a sparse overflow map for levels outside the
// window. When the mid-price drifts out of the central half of the window the
// book re-centers: it rebuilds the ladder around the new mid.
//
// Prices are quantized down to a multiple of `tick` (default 100 = $0.01).
class OrderBook {
 public:
  explicit OrderBook(uint32_t tick = 100, uint32_t window = 4096);

  void add(uint64_t ref, Side side, uint32_t shares, uint32_t price);
  void reduce(uint64_t ref, uint32_t shares);  // execution or partial cancel
  void remove(uint64_t ref);                   // full delete
  void replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price);

  std::optional<Level> best_bid() const;
  std::optional<Level> best_ask() const;
  // Top `n` levels, best first (bids descending, asks ascending).
  std::vector<Level> depth(Side side, size_t n) const;

  size_t order_count() const { return orders_.size(); }
  uint64_t recenter_count() const { return recenters_; }
  uint64_t unknown_ref_count() const { return unknown_refs_; }

 private:
  struct Order { uint32_t price; uint32_t shares; Side side; };
  struct SideBook {
    std::vector<uint64_t> ladder;
    std::map<uint32_t, uint64_t> far;  // levels outside the window
    bool has_best = false;
    uint32_t best = 0;
  };

  bool in_window(uint32_t px) const {
    return px >= base_ && uint64_t(px) < uint64_t(base_) + uint64_t(window_) * tick_;
  }
  size_t idx(uint32_t px) const { return (px - base_) / tick_; }
  uint64_t qty_at(const SideBook& sb, uint32_t px) const;

  void level_add(Side s, uint32_t px, uint64_t q);
  void level_sub(Side s, uint32_t px, uint64_t q);
  void recompute_best(Side s);
  void maybe_recenter();
  void recenter(uint32_t center);

  uint32_t tick_;
  uint32_t window_;
  uint32_t base_ = 0;
  bool initialized_ = false;
  SideBook sides_[2];
  std::unordered_map<uint64_t, Order> orders_;
  uint64_t recenters_ = 0;
  uint64_t unknown_refs_ = 0;
};

}  // namespace itch
