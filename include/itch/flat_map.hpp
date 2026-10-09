#pragma once
// Open-addressing hash map from uint64_t to V, built for the order table:
// one contiguous array, linear probing, backward-shift deletion (no
// tombstones). A lookup touches one cache line in the common case, versus a
// bucket array plus a heap node for std::unordered_map.
//
// Hash: the key is used directly as the slot index (key & mask). ITCH order
// references are close to sequential, so consecutive orders land in
// neighbouring slots and stay cache-friendly. A multiplicative (Fibonacci)
// hash was measured on real data and was slower at the median because it
// scatters those neighbours. The price is that keys with power-of-two strides
// would cluster; do not reuse this map for arbitrary keys.
//
// Constraints: the key ~0ull is reserved as the "empty" marker (ITCH order
// references never reach it). Pointers returned by find()/insert() are
// invalidated by any later insert() or erase().
#include <cstddef>
#include <cstdint>
#include <vector>

namespace itch {

template <class V>
class FlatU64Map {
 public:
  static constexpr uint64_t kEmpty = ~0ull;

  explicit FlatU64Map(size_t min_capacity = 1024) { reset(round_up_pow2(min_capacity)); }

  // Makes room for n entries without growing (load factor stays <= 0.5).
  void reserve(size_t n) {
    const size_t cap = round_up_pow2(n * 2);
    if (cap > slots_.size()) rehash(cap);
  }

  size_t size() const { return size_; }

  V* find(uint64_t k) {
    for (size_t i = pos(k);; i = (i + 1) & mask_) {
      if (slots_[i].key == k) return &slots_[i].val;
      if (slots_[i].key == kEmpty) return nullptr;
    }
  }
  const V* find(uint64_t k) const { return const_cast<FlatU64Map*>(this)->find(k); }

  // Inserts, or overwrites if the key already exists.
  V& insert(uint64_t k, const V& v) {
    if ((size_ + 1) * 2 > slots_.size()) rehash(slots_.size() * 2);
    size_t i = pos(k);
    while (slots_[i].key != kEmpty && slots_[i].key != k) i = (i + 1) & mask_;
    if (slots_[i].key == kEmpty) {
      slots_[i].key = k;
      ++size_;
    }
    slots_[i].val = v;
    return slots_[i].val;
  }

  bool erase(uint64_t k) {
    size_t i = pos(k);
    for (;; i = (i + 1) & mask_) {
      if (slots_[i].key == k) break;
      if (slots_[i].key == kEmpty) return false;
    }
    // Backward-shift deletion (Knuth, algorithm R): pull later entries of the
    // probe run back into the hole unless that would move them before their
    // ideal position.
    size_t j = i;
    for (;;) {
      j = (j + 1) & mask_;
      if (slots_[j].key == kEmpty) break;
      const size_t ideal = pos(slots_[j].key);
      const bool stays = (i <= j) ? (i < ideal && ideal <= j) : (i < ideal || ideal <= j);
      if (stays) continue;
      slots_[i] = slots_[j];
      i = j;
    }
    slots_[i].key = kEmpty;
    --size_;
    return true;
  }

 private:
  struct Slot {
    uint64_t key;
    V val;
  };

  static size_t round_up_pow2(size_t n) {
    size_t c = 16;
    while (c < n) c <<= 1;
    return c;
  }

  size_t pos(uint64_t k) const { return static_cast<size_t>(k) & mask_; }

  void reset(size_t cap) {
    slots_.assign(cap, Slot{kEmpty, V{}});
    mask_ = cap - 1;
    size_ = 0;
  }

  void rehash(size_t new_cap) {
    std::vector<Slot> old;
    old.swap(slots_);
    reset(new_cap);
    for (const Slot& s : old) {
      if (s.key == kEmpty) continue;
      size_t i = pos(s.key);
      while (slots_[i].key != kEmpty) i = (i + 1) & mask_;
      slots_[i] = s;
      ++size_;
    }
  }

  std::vector<Slot> slots_;
  size_t mask_ = 0;
  size_t size_ = 0;
};

}  // namespace itch
