#pragma once
// Hypothetical ("shadow") limit orders replayed against real ITCH events.
//
// A shadow order joins the back of a price level's queue. It never changes the
// real book (no market impact); it only watches what happens to the orders ahead
// of it. Three fill models are tracked side by side:
//
//   touch       optimistic: filled as soon as anything executes at its price or deeper
//   queue-aware filled only when the shares ahead of it are used up (or its level is swept)
//   strict      pessimistic: filled only when the market trades strictly deeper than its price
//
// "Ahead" is exact: an order is ahead of the shadow iff its sequence number is lower
// than the shadow's join sequence, so replacements (which get a new, later sequence
// number) correctly lose priority.
#include <algorithm>
#include <cstdint>
#include <vector>

#include "itch/queue_book.hpp"
#include "itch/types.hpp"

namespace itch {

struct Shadow {
  Side side;
  uint32_t price;
  uint32_t size;
  uint32_t remaining;
  uint64_t seq_join;
  uint64_t ahead;           // shares still queued ahead
  uint64_t ahead_at_join;
  uint64_t join_ts;         // ns since midnight
  uint64_t touch_ts = 0;    // 0 = never
  uint64_t strict_ts = 0;
  uint64_t fill_ts = 0;     // queue-aware, complete fill
};

class ShadowTracker {
 public:
  size_t join(Side side, uint32_t price, uint32_t size, uint64_t seq_join, uint64_t ahead, uint64_t ts) {
    Shadow s{};
    s.side = side;
    s.price = price;
    s.size = size;
    s.remaining = size;
    s.seq_join = seq_join;
    s.ahead = ahead;
    s.ahead_at_join = ahead;
    s.join_ts = ts;
    shadows_.push_back(s);
    active_.push_back(shadows_.size() - 1);
    return shadows_.size() - 1;
  }

  // A resting order was (partly) executed. `before` is its state before the execution.
  void on_execute(const QueueBook::OrderInfo& before, uint32_t executed, uint64_t ts) {
    for (size_t k : active_) {
      Shadow& s = shadows_[k];
      if (s.side != before.side) continue;
      const bool bid = s.side == Side::Bid;
      const bool at = before.price == s.price;
      const bool deeper = bid ? before.price < s.price : before.price > s.price;
      if ((at || deeper) && s.touch_ts == 0) s.touch_ts = ts;
      if (deeper && s.strict_ts == 0) s.strict_ts = ts;
      if (s.fill_ts != 0) continue;
      if (deeper) {                       // the trade swept through our level
        s.remaining = 0;
        s.fill_ts = ts;
      } else if (at) {
        if (before.seq < s.seq_join) {    // an order ahead of us was hit
          s.ahead -= std::min<uint64_t>(s.ahead, executed);
        } else {                          // everything ahead is gone: we are at the front
          if (s.ahead != 0) {
            ++front_violations_;
            if (!credit_out_of_priority_) continue;   // strict FIFO: ignore executions behind older orders
          }
          s.ahead = 0;
          s.remaining -= std::min(executed, s.remaining);
          if (s.remaining == 0) s.fill_ts = ts;
        }
      }
    }
  }

  // Shares left a level without being traded: a (partial) cancel, a delete, or the old
  // half of a replace. Only orders ahead of a shadow move it forward.
  void on_reduce(const QueueBook::OrderInfo& before, uint32_t delta) {
    for (size_t k : active_) {
      Shadow& s = shadows_[k];
      if (s.side == before.side && s.price == before.price && before.seq < s.seq_join)
        s.ahead -= std::min<uint64_t>(s.ahead, delta);
    }
  }

  // Stop tracking shadows that joined at or before `cutoff_ts` (their horizon is over).
  void retire(uint64_t cutoff_ts) {
    active_.erase(std::remove_if(active_.begin(), active_.end(),
                                 [&](size_t k) { return shadows_[k].join_ts <= cutoff_ts; }),
                  active_.end());
  }

  const std::vector<Shadow>& all() const { return shadows_; }
  size_t active_count() const { return active_.size(); }
  const std::vector<size_t>& active() const { return active_; }
  uint64_t front_violations() const { return front_violations_; }
  // Default true: an execution behind the shadow while shares are still ahead counts as a fill
  // for the shadow. false = strict FIFO (lower bound for the queue-aware model).
  void set_credit_out_of_priority(bool v) { credit_out_of_priority_ = v; }

 private:
  std::vector<Shadow> shadows_;
  std::vector<size_t> active_;
  uint64_t front_violations_ = 0;
  bool credit_out_of_priority_ = true;
};

}  // namespace itch
