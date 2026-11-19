// Tests for QueueBook and the shadow-order fill models. No framework; exit code != 0 on failure.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <random>
#include <vector>

#include "itch/order_book.hpp"
#include "itch/queue_book.hpp"
#include "itch/queue_sim.hpp"

using namespace itch;

static int g_fail = 0;
#define CHECK(c)                                                                \
  do {                                                                          \
    if (!(c)) {                                                                 \
      std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                                 \
    }                                                                           \
  } while (0)

static const uint32_t P = 1000000;  // $100.00

// ------------------------------------------------------------------ the queue itself
static void test_queue_basics() {
  QueueBook b;
  b.add(1, Side::Bid, 100, P);
  b.add(2, Side::Bid, 200, P);
  b.add(3, Side::Bid, 50, P);
  b.add(4, Side::Bid, 70, P - 100);
  b.add(5, Side::Ask, 30, P + 100);

  auto q = b.queue(Side::Bid, P);
  CHECK(q.size() == 3 && q[0].ref == 1 && q[1].ref == 2 && q[2].ref == 3);
  CHECK(b.level_qty(Side::Bid, P) == 350);
  CHECK(b.shares_ahead(1) == 0 && b.shares_ahead(2) == 100 && b.shares_ahead(3) == 300);
  CHECK(*b.best_bid() == (Level{P, 350}) && *b.best_ask() == (Level{P + 100, 30}));

  b.reduce(2, 50);  // partial cancel keeps its place
  q = b.queue(Side::Bid, P);
  CHECK(q.size() == 3 && q[1].ref == 2 && q[1].shares == 150);
  CHECK(b.shares_ahead(3) == 250);

  b.remove(1);  // delete from the front
  CHECK(b.shares_ahead(2) == 0 && b.level_qty(Side::Bid, P) == 200);

  b.replace(2, 6, 150, P);  // same price: still loses priority, goes behind order 3
  q = b.queue(Side::Bid, P);
  CHECK(q.size() == 2 && q[0].ref == 3 && q[1].ref == 6);
  CHECK(b.shares_ahead(6) == 50);

  QueueBook::OrderInfo info;
  CHECK(b.reduce(3, 1000, &info) && info.shares == 50 && info.price == P);  // clamped; order 3 gone
  CHECK(!b.lookup(3, info));
  CHECK(b.shares_ahead(6) == 0);

  b.remove(6);
  CHECK(b.queue(Side::Bid, P).empty() && b.level_qty(Side::Bid, P) == 0);
  CHECK(*b.best_bid() == (Level{P - 100, 70}));
  CHECK(!b.reduce(999, 1) && b.unknown_ref_count() == 1);
}

// ------------------------------------------------------------------ cross-check against OrderBook
static void test_matches_order_book() {
  std::mt19937_64 rng(2024);
  QueueBook qb;
  OrderBook ob;
  std::vector<uint64_t> live;
  uint64_t next = 1;
  auto price = [&]() { return uint32_t(P + (int64_t(rng() % 61) - 30) * 100); };
  for (int step = 0; step < 100000; ++step) {
    const uint32_t a = uint32_t(rng() % 100);
    if (live.size() < 10 || a < 40) {
      const uint64_t r = next++;
      const Side s = (rng() & 1) ? Side::Bid : Side::Ask;
      const uint32_t sh = uint32_t(1 + rng() % 300), px = price();
      qb.add(r, s, sh, px);
      ob.add(r, s, sh, px);
      live.push_back(r);
    } else {
      const size_t k = size_t(rng() % live.size());
      const uint64_t r = live[k];
      if (a < 60) {
        qb.remove(r);
        ob.remove(r);
      } else if (a < 85) {
        const uint32_t sh = uint32_t(1 + rng() % 300);
        qb.reduce(r, sh);
        ob.reduce(r, sh);
      } else {
        const uint64_t nr = next++;
        const uint32_t sh = uint32_t(1 + rng() % 300), px = price();
        qb.replace(r, nr, sh, px);
        ob.replace(r, nr, sh, px);
        live.push_back(nr);
      }
      QueueBook::OrderInfo tmp;
      if (!qb.lookup(live[k], tmp)) {
        live[k] = live.back();
        live.pop_back();
      }
    }
    if (!(qb.best_bid() == ob.best_bid()) || !(qb.best_ask() == ob.best_ask())) {
      std::fprintf(stderr, "BBO mismatch at step %d\n", step);
      ++g_fail;
      return;
    }
    if (step % 20 == 0 && (qb.depth(Side::Bid, 5) != ob.depth(Side::Bid, 5) ||
                           qb.depth(Side::Ask, 5) != ob.depth(Side::Ask, 5))) {
      std::fprintf(stderr, "depth mismatch at step %d\n", step);
      ++g_fail;
      return;
    }
  }
  CHECK(qb.order_count() == ob.order_count());
}

// ------------------------------------------------------------------ fill models, by hand
// Helper: apply an execution of `ref` to the book and tell the tracker.
static void execute(QueueBook& b, ShadowTracker& t, uint64_t ref, uint32_t shares, uint64_t ts) {
  QueueBook::OrderInfo before;
  CHECK(b.reduce(ref, shares, &before));
  t.on_execute(before, std::min(shares, before.shares), ts);
}
static void cancel(QueueBook& b, ShadowTracker& t, uint64_t ref, uint32_t shares) {
  QueueBook::OrderInfo before;
  CHECK(b.reduce(ref, shares, &before));
  t.on_reduce(before, std::min(shares, before.shares));
}
static void erase(QueueBook& b, ShadowTracker& t, uint64_t ref) {
  QueueBook::OrderInfo before;
  CHECK(b.remove(ref, &before));
  t.on_reduce(before, before.shares);
}

static void test_queue_aware_fill() {
  QueueBook b;
  ShadowTracker t;
  b.add(1, Side::Bid, 100, P);  // A
  b.add(2, Side::Bid, 200, P);  // B
  const size_t id = t.join(Side::Bid, P, 100, b.next_seq(), b.level_qty(Side::Bid, P), 1000);
  CHECK(t.all()[id].ahead == 300);
  b.add(3, Side::Bid, 100, P);  // C arrives after us: never ahead of us

  execute(b, t, 1, 100, 2000);  // A is hit
  CHECK(t.all()[id].ahead == 200);
  CHECK(t.all()[id].touch_ts == 2000 && t.all()[id].fill_ts == 0);  // touch already says "filled"; queue-aware does not

  cancel(b, t, 2, 80);  // B cancels part of its size, still ahead
  CHECK(t.all()[id].ahead == 120);
  erase(b, t, 2);       // B leaves entirely
  CHECK(t.all()[id].ahead == 0);

  execute(b, t, 3, 30, 3000);   // C is hit: we were at the front, so we take it first
  CHECK(t.all()[id].remaining == 70 && t.all()[id].fill_ts == 0);
  execute(b, t, 3, 70, 4000);   // the next execution completes us
  CHECK(t.all()[id].remaining == 0 && t.all()[id].fill_ts == 4000);
  CHECK(t.front_violations() == 0);
}

static void test_sweep_and_strict() {
  QueueBook b;
  ShadowTracker t;
  b.add(1, Side::Bid, 100, P);
  b.add(2, Side::Bid, 100, P - 100);  // one tick deeper
  const size_t id = t.join(Side::Bid, P, 100, b.next_seq(), b.level_qty(Side::Bid, P), 0);

  execute(b, t, 2, 50, 500);          // a sell traded at the deeper price, so it passed through our level
  const Shadow& s = t.all()[id];
  CHECK(s.touch_ts == 500 && s.strict_ts == 500 && s.fill_ts == 500 && s.remaining == 0);

  // Ask side mirrors it.
  QueueBook b2;
  ShadowTracker t2;
  b2.add(1, Side::Ask, 100, P);
  b2.add(2, Side::Ask, 100, P + 100);
  const size_t id2 = t2.join(Side::Ask, P, 100, b2.next_seq(), b2.level_qty(Side::Ask, P), 0);
  execute(b2, t2, 1, 100, 10);        // at our price, order ahead: touch only
  CHECK(t2.all()[id2].touch_ts == 10 && t2.all()[id2].strict_ts == 0 && t2.all()[id2].fill_ts == 0);
  execute(b2, t2, 2, 10, 20);         // deeper: strict and a full fill
  CHECK(t2.all()[id2].strict_ts == 20 && t2.all()[id2].fill_ts == 20);
}

static void test_replace_loses_priority() {
  QueueBook b;
  ShadowTracker t;
  b.add(1, Side::Bid, 100, P);
  b.add(2, Side::Bid, 100, P);
  const size_t id = t.join(Side::Bid, P, 100, b.next_seq(), b.level_qty(Side::Bid, P), 0);
  CHECK(t.all()[id].ahead == 200);

  QueueBook::OrderInfo old;
  CHECK(b.replace(1, 3, 100, P, &old));  // order 1 is replaced: it leaves the front and re-enters behind us
  t.on_reduce(old, old.shares);
  CHECK(t.all()[id].ahead == 100);
  CHECK(b.shares_ahead(3) == 100 + 0);   // behind order 2 in the real queue
  CHECK(b.queue(Side::Bid, P).back().ref == 3);
}

// ------------------------------------------------------------------ incremental ahead == list traversal
static void test_tracker_matches_traversal() {
  std::mt19937_64 rng(7);
  QueueBook b;
  ShadowTracker t;
  std::vector<uint64_t> live;
  uint64_t next = 1, ts = 1;
  auto price = [&]() { return uint32_t(P + (int64_t(rng() % 15) - 7) * 100); };
  auto prune = [&](size_t k) {
    QueueBook::OrderInfo tmp;
    if (!b.lookup(live[k], tmp)) {
      live[k] = live.back();
      live.pop_back();
    }
  };
  for (int step = 0; step < 40000; ++step) {
    ++ts;
    const uint32_t a = uint32_t(rng() % 100);
    if (live.size() < 10 || a < 35) {
      const uint64_t r = next++;
      b.add(r, (rng() & 1) ? Side::Bid : Side::Ask, uint32_t(1 + rng() % 600), price());
      live.push_back(r);
    } else if (a < 50) {                         // delete
      const size_t k = size_t(rng() % live.size());
      erase(b, t, live[k]);
      prune(k);
    } else if (a < 62) {                         // partial cancel
      const size_t k = size_t(rng() % live.size());
      cancel(b, t, live[k], uint32_t(1 + rng() % 200));
      prune(k);
    } else if (a < 72) {                         // replace
      const size_t k = size_t(rng() % live.size());
      QueueBook::OrderInfo old;
      const uint64_t nr = next++;
      if (b.replace(live[k], nr, uint32_t(1 + rng() % 300), price(), &old)) {
        t.on_reduce(old, old.shares);
        live[k] = nr;
      }
    } else if (a < 84) {                         // execution: always the front order of a level, as on an exchange
      const Side s = (rng() & 1) ? Side::Bid : Side::Ask;
      const auto lv = b.depth(s, 1);
      if (lv.empty()) continue;
      const auto q = b.queue(s, lv[0].price);
      execute(b, t, q.front().ref, uint32_t(1 + rng() % 120), ts);
      for (size_t k = 0; k < live.size();) {
        QueueBook::OrderInfo tmp;
        if (!b.lookup(live[k], tmp)) {
          live[k] = live.back();
          live.pop_back();
        } else {
          ++k;
        }
      }
    } else {                                     // a shadow joins the back of an existing level
      if (rng() % 4 != 0) continue;              // keep the number of shadows moderate
      const Side s = (rng() & 1) ? Side::Bid : Side::Ask;
      const auto lv = b.depth(s, 2);
      if (lv.empty()) continue;
      const Level l = lv[size_t(rng() % lv.size())];
      t.join(s, l.price, uint32_t(100), b.next_seq(), b.level_qty(s, l.price), ts);
    }

    // After every event: for each shadow still waiting, `ahead` must equal the shares queued
    // ahead of it, counted by walking the real list.
    if (step % 20 == 0) {
      for (const Shadow& s : t.all()) {
        if (s.fill_ts != 0) continue;
        uint64_t truth = 0;
        for (const auto& e : b.queue(s.side, s.price))
          if (e.seq < s.seq_join) truth += e.shares;
        if (truth != s.ahead) {
          std::fprintf(stderr, "ahead mismatch at step %d: tracker %llu, list %llu\n", step,
                       (unsigned long long)s.ahead, (unsigned long long)truth);
          ++g_fail;
          return;
        }
      }
    }
  }
  CHECK(t.front_violations() == 0);
}

int main() {
  test_queue_basics();
  test_matches_order_book();
  test_queue_aware_fill();
  test_sweep_and_strict();
  test_replace_loses_priority();
  test_tracker_matches_traversal();
  if (g_fail) {
    std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
    return 1;
  }
  std::puts("all queue tests passed");
  return 0;
}
