#include "itch/order_book.hpp"

#include <algorithm>
#include <utility>

namespace itch {

OrderBook::OrderBook(uint32_t tick, uint32_t window) : tick_(tick), window_(window) {
  sides_[0].ladder.assign(window_, 0);
  sides_[1].ladder.assign(window_, 0);
  orders_.reserve(1u << 17);
}

uint64_t OrderBook::qty_at(const SideBook& sb, uint32_t px) const {
  if (in_window(px)) return sb.ladder[idx(px)];
  auto it = sb.far.find(px);
  return it == sb.far.end() ? 0 : it->second;
}

void OrderBook::level_add(Side s, uint32_t px, uint64_t q) {
  SideBook& sb = sides_[static_cast<int>(s)];
  if (in_window(px)) sb.ladder[idx(px)] += q;
  else sb.far[px] += q;
  const bool better = !sb.has_best || (s == Side::Bid ? px > sb.best : px < sb.best);
  if (better) {
    sb.has_best = true;
    sb.best = px;
  }
}

void OrderBook::level_sub(Side s, uint32_t px, uint64_t q) {
  SideBook& sb = sides_[static_cast<int>(s)];
  uint64_t now;
  if (in_window(px)) {
    now = (sb.ladder[idx(px)] -= q);
  } else {
    auto it = sb.far.find(px);
    if (it == sb.far.end()) return;
    now = (it->second -= q);
    if (now == 0) sb.far.erase(it);
  }
  if (now == 0 && sb.has_best && sb.best == px) recompute_best(s);
}

void OrderBook::recompute_best(Side s) {
  SideBook& sb = sides_[static_cast<int>(s)];
  const uint32_t old = sb.best;
  const uint64_t top = uint64_t(base_) + uint64_t(window_) * tick_;
  bool found = false;
  uint32_t best = 0;

  if (s == Side::Bid) {
    // Scan the ladder downward from the old best (clamped to the window).
    if (old >= base_) {
      long start = (uint64_t(old) >= top) ? long(window_) - 1 : long(idx(old));
      for (long i = start; i >= 0; --i) {
        if (sb.ladder[size_t(i)]) {
          best = base_ + uint32_t(i) * tick_;
          found = true;
          break;
        }
      }
    }
    if (!sb.far.empty()) {
      const uint32_t f = sb.far.rbegin()->first;
      if (!found || f > best) best = f;
      found = true;
    }
  } else {
    if (uint64_t(old) < top) {
      size_t start = (old < base_) ? 0 : idx(old);
      for (size_t i = start; i < window_; ++i) {
        if (sb.ladder[i]) {
          best = base_ + uint32_t(i) * tick_;
          found = true;
          break;
        }
      }
    }
    if (!sb.far.empty()) {
      const uint32_t f = sb.far.begin()->first;
      if (!found || f < best) best = f;
      found = true;
    }
  }
  sb.has_best = found;
  sb.best = found ? best : 0;
}

void OrderBook::recenter(uint32_t center) {
  std::vector<std::pair<uint32_t, uint64_t>> levels[2];
  for (int s = 0; s < 2; ++s) {
    SideBook& sb = sides_[s];
    for (size_t i = 0; i < window_; ++i) {
      if (sb.ladder[i]) {
        levels[s].emplace_back(base_ + uint32_t(i) * tick_, sb.ladder[i]);
        sb.ladder[i] = 0;
      }
    }
    for (const auto& kv : sb.far) levels[s].emplace_back(kv.first, kv.second);
    sb.far.clear();
  }
  const uint32_t half = (window_ / 2) * tick_;
  base_ = center > half ? center - half : 0;
  base_ -= base_ % tick_;
  initialized_ = true;
  for (int s = 0; s < 2; ++s) {
    SideBook& sb = sides_[s];
    for (const auto& lv : levels[s]) {
      if (in_window(lv.first)) sb.ladder[idx(lv.first)] += lv.second;
      else sb.far[lv.first] += lv.second;
    }
  }
  ++recenters_;
}

void OrderBook::maybe_recenter() {
  const SideBook& b = sides_[0];
  const SideBook& a = sides_[1];
  if (!b.has_best && !a.has_best) return;
  uint32_t center;
  if (b.has_best && a.has_best) center = uint32_t((uint64_t(b.best) + a.best) / 2);
  else center = b.has_best ? b.best : a.best;

  const uint64_t lo = uint64_t(base_) + uint64_t(window_ / 4) * tick_;
  const uint64_t hi = uint64_t(base_) + uint64_t(window_ / 4) * 3 * tick_;
  if (!initialized_ || center < lo || center >= hi) recenter(center);
}

void OrderBook::add(uint64_t ref, Side side, uint32_t shares, uint32_t price) {
  if (shares == 0) return;
  if (orders_.find(ref)) remove(ref);  // defensive: duplicate ref
  price -= price % tick_;
  if (!initialized_) recenter(price);
  Order o;
  o.price = price;
  o.shares = shares;
  o.ask = (side == Side::Ask);
  orders_.insert(ref, o);
  level_add(side, price, shares);
  maybe_recenter();
}

void OrderBook::reduce(uint64_t ref, uint32_t shares) {
  Order* o = orders_.find(ref);
  if (!o) {
    ++unknown_refs_;
    return;
  }
  const uint32_t d = std::min<uint32_t>(shares, o->shares);
  const Side s = o->side();
  const uint32_t px = o->price;
  o->shares = o->shares - d;
  if (o->shares == 0) orders_.erase(ref);
  level_sub(s, px, d);
}

void OrderBook::remove(uint64_t ref) {
  const Order* p = orders_.find(ref);
  if (!p) {
    ++unknown_refs_;
    return;
  }
  const Order o = *p;
  orders_.erase(ref);
  level_sub(o.side(), o.price, o.shares);
}

void OrderBook::replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price) {
  const Order* p = orders_.find(old_ref);
  if (!p) {
    ++unknown_refs_;
    return;
  }
  const Side s = p->side();  // a replace keeps the side
  remove(old_ref);
  add(new_ref, s, shares, price);
}

bool OrderBook::lookup(uint64_t ref, uint32_t& price, Side& side) const {
  const Order* o = orders_.find(ref);
  if (!o) return false;
  price = o->price;
  side = o->side();
  return true;
}

std::optional<Level> OrderBook::best_bid() const {
  const SideBook& sb = sides_[0];
  if (!sb.has_best) return std::nullopt;
  return Level{sb.best, qty_at(sb, sb.best)};
}

std::optional<Level> OrderBook::best_ask() const {
  const SideBook& sb = sides_[1];
  if (!sb.has_best) return std::nullopt;
  return Level{sb.best, qty_at(sb, sb.best)};
}

std::vector<Level> OrderBook::depth(Side side, size_t n) const {
  std::vector<Level> out;
  out.reserve(n);
  const SideBook& sb = sides_[static_cast<int>(side)];
  const uint32_t lo = base_;
  const uint64_t hi = uint64_t(base_) + uint64_t(window_) * tick_;

  if (side == Side::Bid) {  // descending
    for (auto it = sb.far.rbegin(); it != sb.far.rend() && out.size() < n; ++it)
      if (uint64_t(it->first) >= hi) out.push_back({it->first, it->second});
    for (long i = long(window_) - 1; i >= 0 && out.size() < n; --i)
      if (sb.ladder[size_t(i)]) out.push_back({base_ + uint32_t(i) * tick_, sb.ladder[size_t(i)]});
    auto it = sb.far.lower_bound(lo);
    while (it != sb.far.begin() && out.size() < n) {
      --it;
      out.push_back({it->first, it->second});
    }
  } else {  // ascending
    for (auto it = sb.far.begin(); it != sb.far.end() && it->first < lo && out.size() < n; ++it)
      out.push_back({it->first, it->second});
    for (size_t i = 0; i < window_ && out.size() < n; ++i)
      if (sb.ladder[i]) out.push_back({base_ + uint32_t(i) * tick_, sb.ladder[i]});
    for (auto it = sb.far.begin(); it != sb.far.end() && out.size() < n; ++it)
      if (uint64_t(it->first) >= hi) out.push_back({it->first, it->second});
  }
  return out;
}

}  // namespace itch
