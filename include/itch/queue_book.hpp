#pragma once
// Order book that keeps the full time-priority queue of every price level.
//
// Orders live in one contiguous pool and are linked into a FIFO per price level
// with 32-bit indices (an intrusive doubly-linked list): no per-order heap
// allocation, and removing an order from the middle of a queue is O(1). Each
// order gets a monotonically increasing sequence number when it enters the book;
// within a level, a lower sequence number means "ahead in the queue".
//
// This book is for studying queue position. The latency-tuned book used for the
// benchmarks is OrderBook; the two are cross-checked against each other in the tests.
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <vector>

#include "itch/flat_map.hpp"
#include "itch/types.hpp"

namespace itch {

class QueueBook {
 public:
  struct OrderInfo {
    uint64_t seq;
    uint32_t price;
    uint32_t shares;
    Side side;
  };
  struct QueueEntry {
    uint64_t ref;
    uint64_t seq;
    uint32_t shares;
  };

  QueueBook();

  void add(uint64_t ref, Side side, uint32_t shares, uint32_t price);
  // Each mutator can report the order's state *before* the change (for observers).
  bool reduce(uint64_t ref, uint32_t shares, OrderInfo* before = nullptr);  // execution / partial cancel
  bool remove(uint64_t ref, OrderInfo* before = nullptr);                   // full delete
  // Replace = delete + add: the new order goes to the back of the queue (priority lost).
  bool replace(uint64_t old_ref, uint64_t new_ref, uint32_t shares, uint32_t price,
               OrderInfo* old_before = nullptr);
  bool lookup(uint64_t ref, OrderInfo& out) const;

  std::optional<Level> best_bid() const;
  std::optional<Level> best_ask() const;
  std::vector<Level> depth(Side side, size_t n) const;
  uint64_t level_qty(Side side, uint32_t price) const;

  uint64_t next_seq() const { return seq_; }
  size_t order_count() const { return index_.size(); }
  uint64_t unknown_ref_count() const { return unknown_refs_; }

  // The whole queue of a level, front to back.
  std::vector<QueueEntry> queue(Side side, uint32_t price) const;
  // Shares queued ahead of a live order in its level (walks the list).
  uint64_t shares_ahead(uint64_t ref) const;

 private:
  static constexpr uint32_t kNil = ~0u;
  struct Node {
    uint64_t ref;
    uint64_t seq;
    uint32_t price;
    uint32_t shares;
    uint32_t prev;
    uint32_t next;
    uint8_t side;
  };
  struct LevelQ {
    uint32_t head = kNil;
    uint32_t tail = kNil;
    uint64_t qty = 0;
  };

  uint32_t alloc();
  void drop(uint32_t i);  // unlink, fix the level, free the node and its index entry

  std::vector<Node> pool_;
  uint32_t free_head_ = kNil;
  FlatU64Map<uint32_t> index_;  // order reference -> pool index
  std::map<uint32_t, LevelQ> levels_[2];
  uint64_t seq_ = 0;
  uint64_t unknown_refs_ = 0;
};

}  // namespace itch
