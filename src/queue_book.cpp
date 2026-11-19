#include "itch/queue_book.hpp"

#include <algorithm>

namespace itch {

QueueBook::QueueBook() {
  pool_.reserve(1u << 17);
  index_.reserve(1u << 17);
}

uint32_t QueueBook::alloc() {
  if (free_head_ != kNil) {
    const uint32_t i = free_head_;
    free_head_ = pool_[i].next;
    return i;
  }
  pool_.push_back(Node{});
  return static_cast<uint32_t>(pool_.size() - 1);
}

void QueueBook::drop(uint32_t i) {
  Node& n = pool_[i];
  auto& lv_map = levels_[n.side];
  auto it = lv_map.find(n.price);
  LevelQ& lv = it->second;
  if (n.prev != kNil) pool_[n.prev].next = n.next;
  else lv.head = n.next;
  if (n.next != kNil) pool_[n.next].prev = n.prev;
  else lv.tail = n.prev;
  if (lv.head == kNil) lv_map.erase(it);
  index_.erase(n.ref);
  n.next = free_head_;  // reuse `next` as the free-list link
  free_head_ = i;
}

void QueueBook::add(uint64_t ref, Side side, uint32_t shares, uint32_t price) {
  if (shares == 0) return;
  if (index_.find(ref)) remove(ref);  // defensive: duplicate reference
  const uint32_t i = alloc();
  LevelQ& lv = levels_[static_cast<int>(side)][price];
  Node& n = pool_[i];
  n.ref = ref;
  n.seq = seq_++;
  n.price = price;
  n.shares = shares;
  n.side = static_cast<uint8_t>(side);
  n.prev = lv.tail;
  n.next = kNil;
  if (lv.tail != kNil) pool_[lv.tail].next = i;
  else lv.head = i;
  lv.tail = i;
  lv.qty += shares;
  index_.insert(ref, i);
}

bool QueueBook::lookup(uint64_t ref, OrderInfo& out) const {
  const uint32_t* ip = index_.find(ref);
  if (!ip) return false;
  const Node& n = pool_[*ip];
  out = OrderInfo{n.seq, n.price, n.shares, static_cast<Side>(n.side)};
  return true;
}

bool QueueBook::reduce(uint64_t ref, uint32_t shares, OrderInfo* before) {
  const uint32_t* ip = index_.find(ref);
  if (!ip) {
    ++unknown_refs_;
    return false;
  }
  const uint32_t i = *ip;
  Node& n = pool_[i];
  if (before) *before = OrderInfo{n.seq, n.price, n.shares, static_cast<Side>(n.side)};
  const uint32_t d = std::min(shares, n.shares);
  levels_[n.side].find(n.price)->second.qty -= d;
  n.shares -= d;
  if (n.shares == 0) drop(i);
  return true;
}

bool QueueBook::remove(uint64_t ref, OrderInfo* before) {
  const uint32_t* ip = index_.find(ref);
  if (!ip) {
    ++unknown_refs_;
    return false;
  }
  const uint32_t i = *ip;
  Node& n = pool_[i];
  if (before) *before = OrderInfo{n.seq, n.price, n.shares, static_cast<Side>(n.side)};
  levels_[n.side].find(n.price)->second.qty -= n.shares;
  drop(i);
  return true;
}

bool QueueBook::replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price,
                        OrderInfo* old_before) {
  OrderInfo b;
  if (!lookup(old_ref, b)) {
    ++unknown_refs_;
    return false;
  }
  if (old_before) *old_before = b;
  remove(old_ref);
  add(new_ref, b.side, shares, price);  // a replace keeps the side
  return true;
}

std::optional<Level> QueueBook::best_bid() const {
  const auto& m = levels_[0];
  if (m.empty()) return std::nullopt;
  const auto it = std::prev(m.end());
  return Level{it->first, it->second.qty};
}

std::optional<Level> QueueBook::best_ask() const {
  const auto& m = levels_[1];
  if (m.empty()) return std::nullopt;
  return Level{m.begin()->first, m.begin()->second.qty};
}

std::vector<Level> QueueBook::depth(Side side, size_t n) const {
  std::vector<Level> out;
  out.reserve(n);
  const auto& m = levels_[static_cast<int>(side)];
  if (side == Side::Bid) {
    for (auto it = m.rbegin(); it != m.rend() && out.size() < n; ++it) out.push_back({it->first, it->second.qty});
  } else {
    for (auto it = m.begin(); it != m.end() && out.size() < n; ++it) out.push_back({it->first, it->second.qty});
  }
  return out;
}

uint64_t QueueBook::level_qty(Side side, uint32_t price) const {
  const auto& m = levels_[static_cast<int>(side)];
  const auto it = m.find(price);
  return it == m.end() ? 0 : it->second.qty;
}

std::vector<QueueBook::QueueEntry> QueueBook::queue(Side side, uint32_t price) const {
  std::vector<QueueEntry> out;
  const auto& m = levels_[static_cast<int>(side)];
  const auto it = m.find(price);
  if (it == m.end()) return out;
  for (uint32_t i = it->second.head; i != kNil; i = pool_[i].next) out.push_back({pool_[i].ref, pool_[i].seq, pool_[i].shares});
  return out;
}

uint64_t QueueBook::shares_ahead(uint64_t ref) const {
  const uint32_t* ip = index_.find(ref);
  if (!ip) return 0;
  const Node& target = pool_[*ip];
  const auto& m = levels_[target.side];
  uint64_t sum = 0;
  for (uint32_t i = m.find(target.price)->second.head; i != kNil && i != *ip; i = pool_[i].next) sum += pool_[i].shares;
  return sum;
}

}  // namespace itch
