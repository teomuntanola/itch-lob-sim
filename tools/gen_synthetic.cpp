// Generates a synthetic ITCH 5.0 BinaryFILE stream for symbol "TEST" so the
// whole pipeline can be exercised without downloading real NASDAQ data.
//
//   gen_synthetic [count] [seed] [out|-]
//
// Mix: ~45% add, 25% delete, 15% execute, 10% partial cancel, 5% replace.
// Every 100k messages the mid price jumps by +/- $25 to force window re-centering.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {

struct Buf {
  std::vector<uint8_t> b;
  void u8(uint8_t v) { b.push_back(v); }
  void u16(uint16_t v) { u8(uint8_t(v >> 8)); u8(uint8_t(v)); }
  void u32(uint32_t v) { u16(uint16_t(v >> 16)); u16(uint16_t(v)); }
  void u48(uint64_t v) { u16(uint16_t(v >> 32)); u32(uint32_t(v)); }
  void u64(uint64_t v) { u32(uint32_t(v >> 32)); u32(uint32_t(v)); }
  void raw(const char* s, size_t n) { for (size_t i = 0; i < n; ++i) u8(uint8_t(s[i])); }
};

struct Live { uint64_t ref; uint32_t shares; };

}  // namespace

int main(int argc, char** argv) {
  const uint64_t count = argc > 1 ? std::strtoull(argv[1], nullptr, 10) : 1000000ULL;
  const uint64_t seed = argc > 2 ? std::strtoull(argv[2], nullptr, 10) : 42ULL;
  const std::string out = argc > 3 ? argv[3] : "synthetic.itch";

  std::FILE* f;
  if (out == "-") {
#ifdef _WIN32
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    f = stdout;
  } else {
    f = std::fopen(out.c_str(), "wb");
    if (!f) { std::perror(out.c_str()); return 1; }
  }

  std::mt19937_64 rng(seed);
  std::vector<uint8_t> chunk;
  chunk.reserve(1 << 20);
  auto flush_frame = [&](const Buf& m) {
    chunk.push_back(uint8_t(m.b.size() >> 8));
    chunk.push_back(uint8_t(m.b.size()));
    chunk.insert(chunk.end(), m.b.begin(), m.b.end());
    if (chunk.size() > (1u << 20) - 128) {
      std::fwrite(chunk.data(), 1, chunk.size(), f);
      chunk.clear();
    }
  };

  const uint16_t locate = 1;
  uint64_t ts = 34200ULL * 1000000000ULL;  // 09:30:00 in ns since midnight
  uint64_t next_ref = 1;
  int64_t mid = 1000000;  // $100.00 in 1/10000 units

  { // Stock Directory 'R' (39 bytes)
    Buf m;
    m.u8('R'); m.u16(locate); m.u16(0); m.u48(ts);
    m.raw("TEST    ", 8);
    m.u8('Q'); m.u8('N'); m.u32(100); m.u8('Y'); m.u8('C'); m.raw("  ", 2);
    m.u8('P'); m.u8('N'); m.u8('N'); m.u8('1'); m.u8('N'); m.u32(0); m.u8('N');
    flush_frame(m);
  }

  std::vector<Live> live;
  live.reserve(1 << 20);

  auto add_order = [&]() {
    const bool buy = rng() & 1;
    const int64_t off = int64_t(1 + rng() % 20) * 100;
    const uint32_t px = uint32_t(buy ? mid - off : mid + off);
    const uint32_t sh = uint32_t(100 * (1 + rng() % 10));
    const uint64_t ref = next_ref++;
    Buf m;
    m.u8('A'); m.u16(locate); m.u16(0); m.u48(ts);
    m.u64(ref); m.u8(buy ? 'B' : 'S'); m.u32(sh); m.raw("TEST    ", 8); m.u32(px);
    flush_frame(m);
    live.push_back({ref, sh});
  };

  for (uint64_t i = 0; i < count; ++i) {
    ts += 1 + rng() % 2000;
    if (i % 50 == 0) mid += (int64_t(rng() % 3) - 1) * 100;
    if (i > 0 && i % 100000 == 0) {
      mid += (rng() & 1 ? 1 : -1) * 250000;
      if (mid < 600000) mid = 600000;
    }
    if (live.size() < 50) { add_order(); continue; }

    const uint32_t r = uint32_t(rng() % 100);
    const size_t k = size_t(rng() % live.size());
    Live& o = live[k];
    Buf m;
    if (r < 45) {
      add_order();
    } else if (r < 70) {  // Delete
      m.u8('D'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(o.ref);
      flush_frame(m);
      live[k] = live.back(); live.pop_back();
    } else if (r < 85) {  // Execute (partial or full)
      const uint32_t q = (rng() & 1) ? o.shares : uint32_t(1 + rng() % o.shares);
      m.u8('E'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(o.ref); m.u32(q); m.u64(i);
      flush_frame(m);
      o.shares -= q;
      if (o.shares == 0) { live[k] = live.back(); live.pop_back(); }
    } else if (r < 95) {  // Partial cancel
      if (o.shares < 2) continue;
      const uint32_t q = uint32_t(1 + rng() % (o.shares - 1));
      m.u8('X'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(o.ref); m.u32(q);
      flush_frame(m);
      o.shares -= q;
    } else {  // Replace (keeps side; new price near mid on the same side is not tracked, use mid +- off)
      const uint64_t nref = next_ref++;
      const uint32_t sh = uint32_t(100 * (1 + rng() % 10));
      const uint32_t px = uint32_t(mid + (int64_t(rng() % 41) - 20) * 100);
      m.u8('U'); m.u16(locate); m.u16(0); m.u48(ts); m.u64(o.ref); m.u64(nref); m.u32(sh); m.u32(px);
      flush_frame(m);
      o.ref = nref; o.shares = sh;
    }
  }

  std::fwrite(chunk.data(), 1, chunk.size(), f);
  if (f != stdout) std::fclose(f);
  return 0;
}
