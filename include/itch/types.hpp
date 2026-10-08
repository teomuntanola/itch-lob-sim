#pragma once
#include <cstdint>

namespace itch {

enum class Side : uint8_t { Bid = 0, Ask = 1 };

// Price is fixed-point with 4 decimals (ITCH convention): 1234500 == $123.4500
struct Level {
  uint32_t price;
  uint64_t qty;
  bool operator==(const Level& o) const { return price == o.price && qty == o.qty; }
};

}  // namespace itch
