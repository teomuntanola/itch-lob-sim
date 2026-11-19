// How often would a limit order resting at the best bid / best ask actually have been filled?
//
// Every --every seconds of market time a hypothetical order is placed at the back of the queue at the
// best bid and at the best ask, then watched for --horizon seconds against the real ITCH events.
// Three fill models are compared (see queue_sim.hpp): touch (optimistic), queue-aware, strict (pessimistic).
//
//   gzip -dc 01302019.NASDAQ_ITCH50.gz | queue_study --symbol AAPL --from 09:35:00 --to 15:55:00 -
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "itch/parser.hpp"
#include "itch/queue_book.hpp"
#include "itch/queue_sim.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace itch;

namespace {

uint64_t parse_clock(const char* text) {
  unsigned h = 0, m = 0, s = 0;
  if (std::sscanf(text, "%u:%u:%u", &h, &m, &s) != 3 || h > 23 || m > 59 || s > 59) {
    std::fprintf(stderr, "bad time '%s' (expected HH:MM:SS)\n", text);
    std::exit(2);
  }
  return (uint64_t(h) * 3600 + uint64_t(m) * 60 + s) * 1000000000ULL;
}

struct Study {
  std::string symbol;
  char sym[8];
  uint64_t from_ns = 0, to_ns = ~0ULL, every_ns = 1000000000ULL, horizon_ns = 300ULL * 1000000000ULL;
  uint32_t size = 100;

  QueueBook book;
  ShadowTracker tracker;
  bool found = false;
  uint16_t locate = 0;
  uint64_t next_spawn = 0, last_ts = 0, messages = 0, applied = 0;

  void init() {
    std::memset(sym, ' ', sizeof(sym));
    std::memcpy(sym, symbol.data(), std::min<size_t>(symbol.size(), sizeof(sym)));
  }
  bool mine(uint16_t loc) const { return found && loc == locate; }

  // Called before every event of our symbol: place new shadow orders, retire old ones.
  void tick(uint64_t ts) {
    last_ts = ts;
    if (ts >= from_ns && ts <= to_ns) {
      if (next_spawn == 0) next_spawn = from_ns;
      if (ts >= next_spawn) {
        for (Side s : {Side::Bid, Side::Ask}) {
          const auto best = s == Side::Bid ? book.best_bid() : book.best_ask();
          if (best) tracker.join(s, best->price, size, book.next_seq(), book.level_qty(s, best->price), ts);
        }
        while (next_spawn <= ts) next_spawn += every_ns;   // one spawn per interval, even after a quiet gap
      }
    }
    if (ts > horizon_ns) tracker.retire(ts - horizon_ns);
  }

  void on(const StockDirectory& m) {
    if (!found && std::memcmp(m.stock, sym, sizeof(sym)) == 0) {
      found = true;
      locate = m.locate;
    }
  }
  void on(const AddOrder& m) {
    if (!mine(m.locate)) return;
    tick(m.ts);
    ++applied;
    book.add(m.ref, m.side, m.shares, m.price);
  }
  void on(const OrderExecuted& m) {
    if (!mine(m.locate)) return;
    tick(m.ts);
    ++applied;
    QueueBook::OrderInfo b;
    if (book.reduce(m.ref, m.shares, &b)) {
      if (audit) audit_execution(b, m);
      tracker.on_execute(b, std::min(m.shares, b.shares), m.ts);
    }
  }
  // --audit: for every shadow that the tracker thinks still has shares ahead while an order
  // BEHIND it executes, compare the tracker's `ahead` with the real queue (walked from the list).
  bool audit = false;
  uint64_t aud_total = 0, aud_real = 0, aud_drift = 0, aud_shown = 0;
  void audit_execution(const QueueBook::OrderInfo& b, const OrderExecuted& m) {
    for (size_t k : tracker.active()) {
      const Shadow& s = tracker.all()[k];
      if (s.side != b.side || s.price != b.price || s.fill_ts != 0) continue;
      if (b.seq < s.seq_join || s.ahead == 0) continue;
      ++aud_total;
      uint64_t truth = 0;
      const auto q = book.queue(b.side, b.price);
      for (const auto& e : q) if (e.seq < s.seq_join) truth += e.shares;
      const bool real = truth > 0;                     // orders really are still ahead
      if (real) ++aud_real; else ++aud_drift;          // truth == 0: our counter is stale
      if (aud_shown < 6) {
        ++aud_shown;
        std::printf("[audit] t=%.6f s  ref=%llu exec=%u  tracker.ahead=%llu  real_ahead=%llu  (%s)\n  queue front:",
                    m.ts / 1e9, (unsigned long long)m.ref, m.shares, (unsigned long long)s.ahead,
                    (unsigned long long)truth, real ? "REAL out-of-priority execution" : "TRACKER STALE");
        for (size_t i = 0; i < q.size() && i < 4; ++i)
          std::printf(" [ref %llu, %u sh, seq %llu]", (unsigned long long)q[i].ref, q[i].shares, (unsigned long long)q[i].seq);
        std::printf("  | shadow seq_join=%llu, executed order seq=%llu\n", (unsigned long long)s.seq_join, (unsigned long long)b.seq);
      }
    }
  }
  void on(const OrderCancel& m) {
    if (!mine(m.locate)) return;
    tick(m.ts);
    ++applied;
    QueueBook::OrderInfo b;
    if (book.reduce(m.ref, m.shares, &b)) tracker.on_reduce(b, std::min(m.shares, b.shares));
  }
  void on(const OrderDelete& m) {
    if (!mine(m.locate)) return;
    tick(m.ts);
    ++applied;
    QueueBook::OrderInfo b;
    if (book.remove(m.ref, &b)) tracker.on_reduce(b, b.shares);
  }
  void on(const OrderReplace& m) {
    if (!mine(m.locate)) return;
    tick(m.ts);
    ++applied;
    QueueBook::OrderInfo b;
    if (book.replace(m.old_ref, m.new_ref, m.shares, m.price, &b)) tracker.on_reduce(b, b.shares);
  }

  void process(const uint8_t* msg, size_t len) {
    ++messages;
    parse_message(msg, len, *this);
  }
};

double pct(size_t n, size_t d) { return d ? 100.0 * double(n) / double(d) : 0.0; }

}  // namespace

int main(int argc, char** argv) {
  Study st;
  std::string path;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&]() -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", a.c_str());
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--symbol") st.symbol = next();
    else if (a == "--from") st.from_ns = parse_clock(next());
    else if (a == "--to") st.to_ns = parse_clock(next());
    else if (a == "--every") st.every_ns = uint64_t(std::atof(next()) * 1e9);
    else if (a == "--audit") st.audit = true;
    else if (a == "--no-credit") st.tracker.set_credit_out_of_priority(false);
    else if (a == "--size") st.size = uint32_t(std::atoi(next()));
    else if (a == "--horizon") st.horizon_ns = uint64_t(std::atof(next()) * 1e9);
    else if (a == "-h" || a == "--help") path = "";
    else path = a;
  }
  if (st.symbol.empty() || path.empty() || st.every_ns == 0 || st.size == 0) {
    std::fputs("usage: queue_study --symbol SYM [--from HH:MM:SS] [--to HH:MM:SS] [--every SEC] [--size N]\n"
               "                   [--horizon SEC] [--audit] [--no-credit] <itch-file|->\n", stderr);
    return 2;
  }
  st.init();
  if (st.from_ns == 0) st.from_ns = parse_clock("09:35:00");
  if (st.to_ns == ~0ULL) st.to_ns = parse_clock("15:55:00");

  std::FILE* f = stdin;
  if (path != "-") {
    f = std::fopen(path.c_str(), "rb");
    if (!f) {
      std::perror(path.c_str());
      return 1;
    }
  }
#ifdef _WIN32
  else _setmode(_fileno(stdin), _O_BINARY);
#endif
  read_frames(f, [&](const uint8_t* m, size_t n) { st.process(m, n); });
  if (f != stdin) std::fclose(f);

  // Only shadows whose full horizon fits inside the data are scored, so every one had the same chance to fill.
  const auto& all = st.tracker.all();
  std::vector<const Shadow*> scored;
  for (const Shadow& s : all)
    if (s.join_ts + st.horizon_ns <= st.last_ts) scored.push_back(&s);

  std::printf("symbol %s | %llu messages read, %llu applied to the book | live orders at end: %zu | unknown refs: %llu\n",
              st.symbol.c_str(), (unsigned long long)st.messages, (unsigned long long)st.applied,
              st.book.order_count(), (unsigned long long)st.book.unknown_ref_count());
  std::printf("hypothetical orders: %u shares, one per side every %.3g s from %llu to %llu s of market time, "
              "scored over %.0f s: %zu of %zu\n",
              st.size, double(st.every_ns) / 1e9, (unsigned long long)(st.from_ns / 1000000000ULL),
              (unsigned long long)(st.to_ns / 1000000000ULL), double(st.horizon_ns) / 1e9, scored.size(), all.size());
  if (scored.empty()) {
    std::puts("nothing to score: was the symbol found and is the time window inside the data?");
    return 0;
  }

  std::vector<uint64_t> ahead;
  for (const Shadow* s : scored) ahead.push_back(s->ahead_at_join);
  std::sort(ahead.begin(), ahead.end());
  std::printf("shares ahead at join: median %llu, p90 %llu\n", (unsigned long long)ahead[ahead.size() / 2],
              (unsigned long long)ahead[ahead.size() * 9 / 10]);
  std::printf("front-of-queue inconsistencies (an execution behind the shadow while shares were still ahead): %llu\n\n",
              (unsigned long long)st.tracker.front_violations());
  if (st.audit)
    std::printf("audit: %llu shadow checks | real orders still ahead (out-of-priority execution): %llu | tracker stale: %llu\n\n",
                (unsigned long long)st.aud_total, (unsigned long long)st.aud_real, (unsigned long long)st.aud_drift);

  std::puts("share of orders filled within T seconds of joining the queue:");
  std::puts("     T    touch (optimistic)   queue-aware   strict (pessimistic)");
  for (double T : {1.0, 10.0, 30.0, 60.0, 120.0, 300.0}) {
    const uint64_t lim = uint64_t(T * 1e9);
    if (lim > st.horizon_ns) continue;
    size_t touch = 0, queue = 0, strict = 0;
    for (const Shadow* s : scored) {
      if (s->touch_ts && s->touch_ts - s->join_ts <= lim) ++touch;
      if (s->fill_ts && s->fill_ts - s->join_ts <= lim) ++queue;
      if (s->strict_ts && s->strict_ts - s->join_ts <= lim) ++strict;
    }
    std::printf("%5.0f s   %17.1f %%   %9.1f %%   %17.1f %%\n", T, pct(touch, scored.size()),
                pct(queue, scored.size()), pct(strict, scored.size()));
  }
  return 0;
}
