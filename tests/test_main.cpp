// Dependency-free tests (no framework). Exit code != 0 on any failure.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "itch/flat_map.hpp"
#include "itch/latency.hpp"
#include "itch/order_book.hpp"
#include "itch/parser.hpp"
#include "itch/pipeline.hpp"

using namespace itch;

static int g_fail = 0;
#define CHECK(c)                                                              \
  do {                                                                        \
    if (!(c)) {                                                               \
      std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); \
      ++g_fail;                                                               \
    }                                                                         \
  } while (0)

// ---------- helpers to build ITCH messages ----------
struct Buf {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
  void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
  void u48(uint64_t v) { u16(uint16_t(v >> 32)); u32(uint32_t(v)); }
  void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
  void stock(const char* s) {
    char p[8];
    std::memset(p, ' ', 8);
    std::memcpy(p, s, std::min<size_t>(std::strlen(s), 8));
    for (char c : p) u8(uint8_t(c));
  }
};

static Buf msg_add(uint16_t loc, uint64_t ts, uint64_t ref, char side, uint32_t sh, const char* st, uint32_t px) {
  Buf m;
  m.u8('A'); m.u16(loc); m.u16(0); m.u48(ts); m.u64(ref); m.u8(side); m.u32(sh); m.stock(st); m.u32(px);
  return m;
}
static Buf msg_delete(uint16_t loc, uint64_t ts, uint64_t ref) {
  Buf m;
  m.u8('D'); m.u16(loc); m.u16(0); m.u48(ts); m.u64(ref);
  return m;
}
static Buf msg_exec(uint16_t loc, uint64_t ts, uint64_t ref, uint32_t sh) {
  Buf m;
  m.u8('E'); m.u16(loc); m.u16(0); m.u48(ts); m.u64(ref); m.u32(sh); m.u64(999);
  return m;
}
static Buf msg_cancel(uint16_t loc, uint64_t ts, uint64_t ref, uint32_t sh) {
  Buf m;
  m.u8('X'); m.u16(loc); m.u16(0); m.u48(ts); m.u64(ref); m.u32(sh);
  return m;
}
static Buf msg_replace(uint16_t loc, uint64_t ts, uint64_t oref, uint64_t nref, uint32_t sh, uint32_t px) {
  Buf m;
  m.u8('U'); m.u16(loc); m.u16(0); m.u48(ts); m.u64(oref); m.u64(nref); m.u32(sh); m.u32(px);
  return m;
}
static Buf msg_dir(uint16_t loc, const char* st) {
  Buf m;
  m.u8('R'); m.u16(loc); m.u16(0); m.u48(0); m.stock(st);
  for (int i = 0; i < 20; ++i) m.u8(0);  // remaining fields -> total 39 bytes
  return m;
}

// ---------- parser ----------
struct Rec {
  std::vector<AddOrder> adds;
  std::vector<OrderExecuted> execs;
  std::vector<OrderCancel> cancels;
  std::vector<OrderDelete> dels;
  std::vector<OrderReplace> reps;
  std::vector<StockDirectory> dirs;
  void on(const AddOrder& m) { adds.push_back(m); }
  void on(const OrderExecuted& m) { execs.push_back(m); }
  void on(const OrderCancel& m) { cancels.push_back(m); }
  void on(const OrderDelete& m) { dels.push_back(m); }
  void on(const OrderReplace& m) { reps.push_back(m); }
  void on(const StockDirectory& m) { dirs.push_back(m); }
};

static void test_parser() {
  Rec r;
  Buf a = msg_add(7, 123456789012ULL, 0x1122334455667788ULL, 'B', 300, "AAPL", 1234500);
  CHECK(a.b.size() == 36);
  CHECK(parse_message(a.b.data(), a.b.size(), r));
  CHECK(r.adds.size() == 1);
  CHECK(r.adds[0].locate == 7 && r.adds[0].ts == 123456789012ULL);
  CHECK(r.adds[0].ref == 0x1122334455667788ULL);
  CHECK(r.adds[0].side == Side::Bid && r.adds[0].shares == 300 && r.adds[0].price == 1234500);

  Buf s = msg_add(7, 1, 2, 'S', 5, "AAPL", 10);
  parse_message(s.b.data(), s.b.size(), r);
  CHECK(r.adds[1].side == Side::Ask);

  Buf e = msg_exec(7, 5, 77, 40);
  CHECK(e.b.size() == 31);
  parse_message(e.b.data(), e.b.size(), r);
  CHECK(r.execs.size() == 1 && r.execs[0].ref == 77 && r.execs[0].shares == 40);

  Buf x = msg_cancel(7, 5, 78, 11);
  CHECK(x.b.size() == 23);
  parse_message(x.b.data(), x.b.size(), r);
  CHECK(r.cancels.size() == 1 && r.cancels[0].ref == 78 && r.cancels[0].shares == 11);

  Buf d = msg_delete(7, 5, 79);
  CHECK(d.b.size() == 19);
  parse_message(d.b.data(), d.b.size(), r);
  CHECK(r.dels.size() == 1 && r.dels[0].ref == 79);

  Buf u = msg_replace(7, 5, 80, 81, 25, 999900);
  CHECK(u.b.size() == 35);
  parse_message(u.b.data(), u.b.size(), r);
  CHECK(r.reps.size() == 1 && r.reps[0].old_ref == 80 && r.reps[0].new_ref == 81 &&
        r.reps[0].shares == 25 && r.reps[0].price == 999900);

  Buf dir = msg_dir(9, "MSFT");
  CHECK(dir.b.size() == 39);
  parse_message(dir.b.data(), dir.b.size(), r);
  CHECK(r.dirs.size() == 1 && r.dirs[0].locate == 9 && std::memcmp(r.dirs[0].stock, "MSFT    ", 8) == 0);

  const uint8_t unk[] = {'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'O'};
  CHECK(!parse_message(unk, sizeof(unk), r));
  CHECK(!parse_message(a.b.data(), 10, r));  // truncated
}

// ---------- read_frames ----------
static void test_read_frames() {
  const char* path = "itch_test_frames.bin";
  std::FILE* f = std::fopen(path, "wb");
  CHECK(f != nullptr);
  if (!f) return;
  const Buf frames[] = {msg_dir(1, "TEST"), msg_add(1, 1, 1, 'B', 100, "TEST", 1000000),
                        msg_delete(1, 2, 1)};
  for (int rep = 0; rep < 20000; ++rep)  // > 1 MiB so the buffer refills several times
    for (const Buf& m : frames) {
      const uint8_t h[2] = {uint8_t(m.b.size() >> 8), uint8_t(m.b.size())};
      std::fwrite(h, 1, 2, f);
      std::fwrite(m.b.data(), 1, m.b.size(), f);
    }
  std::fclose(f);

  f = std::fopen(path, "rb");
  CHECK(f != nullptr);
  if (!f) return;
  uint64_t n = 0, adds = 0;
  read_frames(f, [&](const uint8_t* m, size_t) {
    ++n;
    if (m[0] == 'A') ++adds;
  });
  std::fclose(f);
  std::remove(path);
  CHECK(n == 60000);
  CHECK(adds == 20000);
}

// ---------- flat hash map ----------
static void test_flat_map() {
  FlatU64Map<uint32_t> m(16);
  std::unordered_map<uint64_t, uint32_t> ref;
  std::mt19937_64 rng(777);
  // Small key range => many collisions, wrap-around probe runs and heavy erase/reinsert.
  for (int round = 0; round < 3; ++round) {
    const uint64_t range = round == 0 ? 64 : (round == 1 ? 5000 : 400000);
    for (int step = 0; step < 600000; ++step) {
      const uint64_t k = rng() % range;
      const uint32_t op = uint32_t(rng() % 100);
      if (op < 45) {
        const uint32_t v = uint32_t(rng());
        m.insert(k, v);
        ref[k] = v;
      } else if (op < 85) {
        const bool a = m.erase(k);
        const bool b = ref.erase(k) > 0;
        if (a != b) { CHECK(a == b); return; }
      } else {
        const uint32_t* f = m.find(k);
        auto it = ref.find(k);
        if ((f != nullptr) != (it != ref.end()) || (f && *f != it->second)) {
          CHECK(false);
          return;
        }
      }
      if (m.size() != ref.size()) { CHECK(m.size() == ref.size()); return; }
    }
  }
  for (const auto& kv : ref) {
    const uint32_t* f = m.find(kv.first);
    if (!f || *f != kv.second) { CHECK(false); return; }
  }
}

// ---------- order book ----------
static void test_book_basic() {
  OrderBook b;
  CHECK(!b.best_bid() && !b.best_ask());
  b.add(1, Side::Bid, 100, 1000000);  // 100.00
  b.add(2, Side::Bid, 50, 999900);    // 99.99
  b.add(3, Side::Ask, 70, 1000100);   // 100.01
  b.add(4, Side::Bid, 25, 1000000);   // same level as #1
  CHECK(b.best_bid() && *b.best_bid() == (Level{1000000, 125}));
  CHECK(b.best_ask() && *b.best_ask() == (Level{1000100, 70}));

  b.reduce(1, 40);  // execution
  CHECK(*b.best_bid() == (Level{1000000, 85}));
  b.remove(4);
  CHECK(*b.best_bid() == (Level{1000000, 60}));
  b.remove(1);  // level empties -> best falls to 99.99
  CHECK(*b.best_bid() == (Level{999900, 50}));

  b.replace(3, 5, 90, 1000200);  // moves ask to 100.02, new ref
  CHECK(*b.best_ask() == (Level{1000200, 90}));
  b.reduce(5, 90);
  CHECK(!b.best_ask());
  CHECK(b.order_count() == 1);

  b.reduce(12345, 1);  // unknown ref
  CHECK(b.unknown_ref_count() == 1);

  auto d = b.depth(Side::Bid, 5);
  CHECK(d.size() == 1 && d[0] == (Level{999900, 50}));
}

static void test_book_recenter() {
  OrderBook b;  // window = 4096 ticks = $40.96
  b.add(1, Side::Bid, 100, 1000000);   // 100.00
  b.add(2, Side::Ask, 100, 1000100);   // 100.01
  b.add(3, Side::Ask, 10, 5000000);    // 500.00 -> far
  b.add(4, Side::Bid, 10, 5000);       // 0.50   -> far
  CHECK(*b.best_bid() == (Level{1000000, 100}));
  CHECK(*b.best_ask() == (Level{1000100, 100}));

  b.remove(2);  // best ask is now the far level
  CHECK(*b.best_ask() == (Level{5000000, 10}));

  b.add(5, Side::Bid, 30, 4999900);  // 499.99 -> forces a recenter around the new mid
  CHECK(b.recenter_count() >= 2);
  CHECK(*b.best_bid() == (Level{4999900, 30}));
  CHECK(*b.best_ask() == (Level{5000000, 10}));
  auto bids = b.depth(Side::Bid, 5);
  CHECK(bids.size() == 3);
  CHECK(bids[0].price == 4999900 && bids[1].price == 1000000 && bids[2].price == 5000);
}

// Naive reference model used to cross-check the optimized book.
struct RefBook {
  struct O { Side s; uint32_t px; uint32_t sh; };
  std::map<uint32_t, uint64_t> lv[2];
  std::unordered_map<uint64_t, O> ord;
  static constexpr uint32_t kTick = 100;

  void add(uint64_t ref, Side s, uint32_t sh, uint32_t px) {
    px -= px % kTick;
    ord[ref] = {s, px, sh};
    lv[int(s)][px] += sh;
  }
  void sub(const O& o, uint32_t d) {
    auto& m = lv[int(o.s)];
    auto it = m.find(o.px);
    it->second -= d;
    if (it->second == 0) m.erase(it);
  }
  void reduce(uint64_t ref, uint32_t sh) {
    auto it = ord.find(ref);
    if (it == ord.end()) return;
    const uint32_t d = std::min(sh, it->second.sh);
    sub(it->second, d);
    it->second.sh -= d;
    if (it->second.sh == 0) ord.erase(it);
  }
  void remove(uint64_t ref) {
    auto it = ord.find(ref);
    if (it == ord.end()) return;
    sub(it->second, it->second.sh);
    ord.erase(it);
  }
  void replace(uint64_t oref, uint64_t nref, uint32_t sh, uint32_t px) {
    auto it = ord.find(oref);
    if (it == ord.end()) return;
    const Side s = it->second.s;
    remove(oref);
    add(nref, s, sh, px);
  }
  std::optional<Level> best(Side s) const {
    const auto& m = lv[int(s)];
    if (m.empty()) return std::nullopt;
    auto it = s == Side::Bid ? std::prev(m.end()) : m.begin();
    return Level{it->first, it->second};
  }
  std::vector<Level> depth(Side s, size_t n) const {
    std::vector<Level> out;
    const auto& m = lv[int(s)];
    if (s == Side::Bid) {
      for (auto it = m.rbegin(); it != m.rend() && out.size() < n; ++it) out.push_back({it->first, it->second});
    } else {
      for (auto it = m.begin(); it != m.end() && out.size() < n; ++it) out.push_back({it->first, it->second});
    }
    return out;
  }
};

static void test_book_random_vs_reference() {
  std::mt19937_64 rng(12345);
  OrderBook b;
  RefBook r;
  std::vector<uint64_t> live;
  uint64_t next_ref = 1;
  int64_t center = 1000000;

  auto rand_price = [&]() -> uint32_t {
    if (rng() % 100 < 80) {  // clustered near a slowly drifting center
      const int64_t off = int64_t(rng() % 401) - 200;
      return uint32_t(std::max<int64_t>(100, center + off * 100));
    }
    return uint32_t(100 + (rng() % 5000000));  // anywhere in $0.01..$500
  };
  auto drop_dead = [&](size_t k) {
    if (!r.ord.count(live[k])) {
      live[k] = live.back();
      live.pop_back();
    }
  };

  for (int step = 0; step < 200000; ++step) {
    if (step % 5000 == 0) center += (int64_t(rng() % 2) ? 1 : -1) * 300000;
    if (center < 100000) center = 100000;
    const uint32_t a = uint32_t(rng() % 100);
    if (live.size() < 20 || a < 40) {
      const uint64_t ref = next_ref++;
      const Side s = (rng() & 1) ? Side::Bid : Side::Ask;
      const uint32_t sh = uint32_t(1 + rng() % 500);
      const uint32_t px = rand_price();
      b.add(ref, s, sh, px);
      r.add(ref, s, sh, px);
      live.push_back(ref);
    } else {
      const size_t k = size_t(rng() % live.size());
      const uint64_t ref = live[k];
      if (a < 60) {
        b.remove(ref);
        r.remove(ref);
      } else if (a < 80) {
        const uint32_t sh = uint32_t(1 + rng() % 500);
        b.reduce(ref, sh);
        r.reduce(ref, sh);
      } else {
        const uint64_t nref = next_ref++;
        const uint32_t sh = uint32_t(1 + rng() % 500);
        const uint32_t px = rand_price();
        b.replace(ref, nref, sh, px);
        r.replace(ref, nref, sh, px);
        live.push_back(nref);
      }
      drop_dead(k);
    }
    if (!(b.best_bid() == r.best(Side::Bid)) || !(b.best_ask() == r.best(Side::Ask))) {
      std::fprintf(stderr, "BBO mismatch at step %d\n", step);
      ++g_fail;
      return;
    }
    if (step % 25 == 0) {
      if (b.depth(Side::Bid, 5) != r.depth(Side::Bid, 5) || b.depth(Side::Ask, 5) != r.depth(Side::Ask, 5)) {
        std::fprintf(stderr, "depth mismatch at step %d\n", step);
        ++g_fail;
        return;
      }
    }
  }
  CHECK(b.order_count() == r.ord.size());
  CHECK(b.recenter_count() > 0);
}

// ---------- pipeline ----------
static void test_pipeline() {
  OrderBook book;
  LatencyRecorder rec;
  PipelineOptions opt;
  opt.symbol = "TEST";
  Pipeline p(opt, book, rec);
  auto feed = [&](const Buf& m) { p.process_frame(m.b.data(), m.b.size()); };

  feed(msg_dir(3, "OTHER"));
  feed(msg_dir(5, "TEST"));
  CHECK(p.symbol_found() && p.locate() == 5);

  feed(msg_add(5, 10, 1, 'B', 100, "TEST", 1000000));
  feed(msg_add(5, 11, 2, 'S', 200, "TEST", 1000100));
  feed(msg_add(3, 12, 3, 'B', 999, "OTHER", 2000000));  // other symbol: ignored
  feed(msg_exec(5, 13, 1, 30));
  feed(msg_cancel(5, 14, 2, 50));
  CHECK(*book.best_bid() == (Level{1000000, 70}));
  CHECK(*book.best_ask() == (Level{1000100, 150}));
  CHECK(book.order_count() == 2);
  CHECK(p.symbol_messages() == 4);
  CHECK(rec.count() == 4);

  const uint8_t unsupported[] = {'S', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'O'};
  p.process_frame(unsupported, sizeof(unsupported));
  CHECK(p.unsupported_messages() == 1);
  CHECK(p.total_messages() == 8);

  const LatencySummary s = rec.summarize();
  CHECK(s.count == 4 && s.min <= s.p50 && s.p50 <= s.p99 && s.p99 <= s.max);
}

int main() {
  test_parser();
  test_read_frames();
  test_flat_map();
  test_book_basic();
  test_book_recenter();
  test_book_random_vs_reference();
  test_pipeline();
  if (g_fail) {
    std::fprintf(stderr, "%d check(s) FAILED\n", g_fail);
    return 1;
  }
  std::puts("all tests passed");
  return 0;
}
