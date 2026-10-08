#pragma once
// NASDAQ TotalView-ITCH 5.0 parser (BinaryFILE framing: 2-byte big-endian
// length prefix before every message). All multi-byte fields are big-endian.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "itch/types.hpp"

namespace itch {

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t rd32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
inline uint64_t rd48(const uint8_t* p) {
  return (uint64_t(p[0]) << 40) | (uint64_t(p[1]) << 32) | (uint64_t(p[2]) << 24) |
         (uint64_t(p[3]) << 16) | (uint64_t(p[4]) << 8) | uint64_t(p[5]);
}
inline uint64_t rd64(const uint8_t* p) { return (uint64_t(rd32(p)) << 32) | rd32(p + 4); }

struct StockDirectory { uint16_t locate; char stock[8]; };
struct AddOrder       { uint16_t locate; uint64_t ts; uint64_t ref; Side side; uint32_t shares; uint32_t price; };
struct OrderExecuted  { uint16_t locate; uint64_t ts; uint64_t ref; uint32_t shares; };  // 'E' and 'C'
struct OrderCancel    { uint16_t locate; uint64_t ts; uint64_t ref; uint32_t shares; };  // 'X' (partial cancel)
struct OrderDelete    { uint16_t locate; uint64_t ts; uint64_t ref; };                   // 'D'
struct OrderReplace   { uint16_t locate; uint64_t ts; uint64_t old_ref; uint64_t new_ref; uint32_t shares; uint32_t price; };  // 'U'

// Decodes one message (without the 2-byte frame length) and calls h.on(msg).
// Returns false if the type is not handled (valid but irrelevant, e.g. 'S','H','P')
// or the message is too short.
template <class Handler>
bool parse_message(const uint8_t* p, size_t len, Handler& h) {
  if (len == 0) return false;
  switch (p[0]) {
    case 'R': {
      if (len < 39) return false;
      StockDirectory m;
      m.locate = rd16(p + 1);
      std::memcpy(m.stock, p + 11, 8);
      h.on(m);
      return true;
    }
    case 'A':
    case 'F': {  // 'F' = Add Order with MPID attribution (extra 4 bytes ignored)
      if (len < 36) return false;
      AddOrder m;
      m.locate = rd16(p + 1);
      m.ts = rd48(p + 5);
      m.ref = rd64(p + 11);
      m.side = (p[19] == 'B') ? Side::Bid : Side::Ask;
      m.shares = rd32(p + 20);
      m.price = rd32(p + 32);
      h.on(m);
      return true;
    }
    case 'E':
    case 'C': {  // 'C' = executed with price (price field ignored by the book)
      if (len < 31) return false;
      OrderExecuted m;
      m.locate = rd16(p + 1);
      m.ts = rd48(p + 5);
      m.ref = rd64(p + 11);
      m.shares = rd32(p + 19);
      h.on(m);
      return true;
    }
    case 'X': {
      if (len < 23) return false;
      OrderCancel m;
      m.locate = rd16(p + 1);
      m.ts = rd48(p + 5);
      m.ref = rd64(p + 11);
      m.shares = rd32(p + 19);
      h.on(m);
      return true;
    }
    case 'D': {
      if (len < 19) return false;
      OrderDelete m;
      m.locate = rd16(p + 1);
      m.ts = rd48(p + 5);
      m.ref = rd64(p + 11);
      h.on(m);
      return true;
    }
    case 'U': {
      if (len < 35) return false;
      OrderReplace m;
      m.locate = rd16(p + 1);
      m.ts = rd48(p + 5);
      m.old_ref = rd64(p + 11);
      m.new_ref = rd64(p + 19);
      m.shares = rd32(p + 27);
      m.price = rd32(p + 31);
      h.on(m);
      return true;
    }
    default:
      return false;
  }
}

// Reads a BinaryFILE stream and calls on_frame(msg_ptr, msg_len) per message.
// Returns total bytes read. Works on regular files and pipes (stdin).
template <class OnFrame>
uint64_t read_frames(std::FILE* f, OnFrame&& on_frame) {
  constexpr size_t kCap = 1u << 20;  // far larger than the max frame (65537 bytes)
  std::vector<uint8_t> buf(kCap);
  size_t have = 0;
  uint64_t bytes = 0;
  for (;;) {
    const size_t n = std::fread(buf.data() + have, 1, kCap - have, f);
    have += n;
    bytes += n;
    size_t pos = 0;
    while (have - pos >= 2) {
      const size_t len = rd16(&buf[pos]);
      if (have - pos < 2 + len) break;
      if (len) on_frame(&buf[pos + 2], len);
      pos += 2 + len;
    }
    if (n == 0) break;
    if (pos) {
      std::memmove(buf.data(), buf.data() + pos, have - pos);
      have -= pos;
    }
  }
  return bytes;
}

}  // namespace itch
