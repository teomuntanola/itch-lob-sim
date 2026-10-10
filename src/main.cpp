#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "itch/latency.hpp"
#include "itch/order_book.hpp"
#include "itch/parser.hpp"
#include "itch/pipeline.hpp"

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#ifdef __linux__
#include <sched.h>
#endif

// "HH:MM:SS" -> nanoseconds since midnight; exits on bad input.
static uint64_t parse_clock(const char* text) {
  unsigned h = 0, m = 0, sec = 0;
  if (std::sscanf(text, "%u:%u:%u", &h, &m, &sec) != 3 || h > 23 || m > 59 || sec > 59) {
    std::fprintf(stderr, "bad time '%s' (expected HH:MM:SS)\n", text);
    std::exit(2);
  }
  return (uint64_t(h) * 3600 + uint64_t(m) * 60 + sec) * 1000000000ULL;
}

static void usage() {
  std::fputs(
      "usage: itch_sim --symbol SYM [options] <file|->\n"
      "  --symbol SYM      stock to replay (required), e.g. AAPL\n"
      "  --bbo FILE        write best bid/offer changes to CSV\n"
      "  --depth FILE      write depth snapshots to CSV\n"
      "  --trades FILE     write executions (resting-order price and side) to CSV\n"
      "  --levels N        depth levels per side (default 5)\n"
      "  --depth-ms N      market-time ms between depth snapshots (default 1000)\n"
      "  --from HH:MM:SS   only export CSV rows from this market time (default: whole day)\n"
      "  --to HH:MM:SS     only export CSV rows up to this market time\n"
      "  --pin CPU         pin the process to a CPU core (Linux only)\n"
      "  <file|->          decompressed ITCH 5.0 BinaryFILE, or '-' for stdin\n"
      "                    e.g. gzip -dc 01302019.NASDAQ_ITCH50.gz | itch_sim --symbol AAPL -\n",
      stderr);
}

int main(int argc, char** argv) {
  itch::PipelineOptions opt;
  std::string path;
  int pin = -1;

  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "missing value for %s\n", name);
        std::exit(2);
      }
      return argv[++i];
    };
    if (a == "--symbol") opt.symbol = next("--symbol");
    else if (a == "--bbo") opt.bbo_path = next("--bbo");
    else if (a == "--depth") opt.depth_path = next("--depth");
    else if (a == "--trades") opt.trades_path = next("--trades");
    else if (a == "--from") opt.export_from_ns = parse_clock(next("--from"));
    else if (a == "--to") opt.export_to_ns = parse_clock(next("--to"));
    else if (a == "--levels") opt.depth_levels = std::strtoull(next("--levels"), nullptr, 10);
    else if (a == "--depth-ms") opt.depth_interval_ns = std::strtoull(next("--depth-ms"), nullptr, 10) * 1000000ULL;
    else if (a == "--pin") pin = std::atoi(next("--pin"));
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else path = a;
  }
  if (opt.symbol.empty() || path.empty()) {
    usage();
    return 2;
  }

#ifdef __linux__
  if (pin >= 0) {
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(pin, &set);
    if (sched_setaffinity(0, sizeof(set), &set) != 0) std::perror("sched_setaffinity");
  }
#else
  (void)pin;
#endif

  std::FILE* f = nullptr;
  if (path == "-") {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
#endif
    f = stdin;
  } else {
    f = std::fopen(path.c_str(), "rb");
    if (!f) {
      std::perror(path.c_str());
      return 1;
    }
  }

  const uint64_t overhead = itch::timer_overhead_ns();

  itch::OrderBook book;
  itch::LatencyRecorder rec(1u << 22);
  itch::Pipeline pipe(opt, book, rec);

  const uint64_t t0 = itch::now_ns();
  const uint64_t bytes = itch::read_frames(f, [&](const uint8_t* m, size_t n) { pipe.process_frame(m, n); });
  const uint64_t t1 = itch::now_ns();
  if (f != stdin) std::fclose(f);

  if (!pipe.symbol_found())
    std::fprintf(stderr, "warning: symbol '%s' not found in stock directory messages\n", opt.symbol.c_str());

  const itch::LatencySummary s = rec.summarize();
  const double secs = double(t1 - t0) / 1e9;

  std::printf("symbol            : %s (locate %u)\n", opt.symbol.c_str(), pipe.locate());
  std::printf("messages          : %llu total, %llu for symbol, %llu unsupported types\n",
              (unsigned long long)pipe.total_messages(), (unsigned long long)pipe.symbol_messages(),
              (unsigned long long)pipe.unsupported_messages());
  std::printf("wall time         : %.2f s  (%.2f M msg/s, %.0f MB/s end-to-end incl. I/O)\n", secs,
              secs > 0 ? double(pipe.total_messages()) / secs / 1e6 : 0.0,
              secs > 0 ? double(bytes) / secs / 1e6 : 0.0);
  std::printf("timer overhead    : %llu ns (median of back-to-back clock reads; included in samples)\n",
              (unsigned long long)overhead);
  std::printf("book update (ns)  : n=%llu  min=%u  p50=%u  p90=%u  p99=%u  p99.9=%u  p99.99=%u  max=%u  mean=%.1f\n",
              (unsigned long long)s.count, s.min, s.p50, s.p90, s.p99, s.p999, s.p9999, s.max, s.mean);
  std::printf("book              : %zu live orders, %llu recenters, %llu unknown refs\n", book.order_count(),
              (unsigned long long)book.recenter_count(), (unsigned long long)book.unknown_ref_count());

  const auto bb = book.best_bid();
  const auto ba = book.best_ask();
  std::printf("final BBO         : ");
  if (bb) std::printf("%u.%04u x %llu", bb->price / 10000u, bb->price % 10000u, (unsigned long long)bb->qty);
  else std::printf("-");
  std::printf("  |  ");
  if (ba) std::printf("%u.%04u x %llu", ba->price / 10000u, ba->price % 10000u, (unsigned long long)ba->qty);
  else std::printf("-");
  std::printf("\n");
  return 0;
}
