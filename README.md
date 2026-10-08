# itch-lob-sim

L2 order book simulator in C++20. It replays NASDAQ TotalView-ITCH 5.0 historical data for one symbol, rebuilds the book from per-order (L3) events, measures the latency of every book update, and exports best bid/offer and market depth to CSV.

```
ITCH 5.0 file ──► parser ──► order book (L2 ladder) ──► latency percentiles
                                   └──► BBO / depth CSV
```

## Build

Requires a C++20 compiler (GCC 11+, Clang 14+, MSVC 2022) and CMake 3.20+.

```bash
cmake -S . -B build            # Release by default, tuned for the host CPU
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Pass `-DITCH_NATIVE=OFF` to build without `-march=native` (what CI does).

## Run

### 1. Without real data (synthetic stream)

```bash
./build/gen_synthetic 5000000 42 synth.itch      # 5M messages, seed 42, symbol "TEST"
./build/itch_sim --symbol TEST --bbo bbo.csv --depth depth.csv synth.itch
```

The synthetic stream is for testing and stress only: the mid price jumps every 100k messages to force window re-centering, and old orders are never cleaned up, so the final book can be crossed. Do not quote its numbers.

### 2. With real NASDAQ data

NASDAQ publishes free sample ITCH 5.0 files (one trading day, several GB compressed). Download one and stream it, without decompressing to disk:

```bash
gzip -dc 01302019.NASDAQ_ITCH50.gz | ./build/itch_sim --symbol AAPL --bbo aapl_bbo.csv --depth aapl_depth.csv -
```

Options: `--levels N` (depth levels, default 5), `--depth-ms N` (market-time interval between depth snapshots, default 1000), `--pin CPU` (pin to a core, Linux only).

### Output

```
symbol            : AAPL (locate 13)
messages          : ... total, ... for symbol, ... unsupported types
wall time         : ...
book update (ns)  : n=...  min=...  p50=...  p90=...  p99=...  p99.9=...  p99.99=...  max=...
book              : ... live orders, ... recenters, ... unknown refs
final BBO         : ...
```

`bbo.csv`: `ts_ns,bid_px,bid_qty,ask_px,ask_qty` (a row each time the BBO changes; `ts_ns` is nanoseconds since midnight).
`depth.csv`: `ts_ns,side,level,price,qty` (long format, top N levels per side).

## Design

- **Parser** (`include/itch/parser.hpp`): decodes ITCH 5.0 messages `R A F E C X D U` (big-endian, BinaryFILE framing with a 2-byte length prefix). Everything else is counted as unsupported and skipped. `read_frames` works on files and pipes.
- **Order book** (`include/itch/order_book.hpp`): per-order hash map for O(1) lookup by order reference; per-side dense price ladder over a window of 4096 ticks ($40.96 at $0.01 tick) plus a sparse overflow map for levels outside it. When the mid leaves the central half of the window, the book **re-centers** (rebuilds the ladder around the new mid). The best price is cached per side and only recomputed when the best level empties.
- **Latency** (`include/itch/latency.hpp`): two monotonic clock reads around each book update; all samples are stored and exact percentiles (nearest-rank) are computed at the end. CSV export happens after the stop timestamp, so it is not in the measurement. The measured timer overhead is printed and is included in every sample.
- **Tests** (`tests/test_main.cpp`): parser field decoding, stream framing across buffer refills, book operations, re-centering, and a randomized cross-check of 200k operations against a naive `std::map` reference book (best bid/offer every step, depth every 25 steps).

## Limitations

- Single symbol per run; one tick size for the whole book (default $0.01, prices are quantized down to it, so sub-penny orders are merged into the penny below).
- Only add / execute / cancel / delete / replace messages are applied. Trade messages (`P`, `Q`, `B`), halts, and auction/cross events are ignored.
- `std::unordered_map` for the order map is a deliberate first version; at hundreds of thousands of live orders its cache misses dominate p50. Replacing it is the obvious next optimization.

## Benchmarks

Replay of the NASDAQ ITCH 5.0 sample file `01302019.NASDAQ_ITCH50` (30 Jan 2019), symbol AAPL: 368,366,634 messages in the file, 1,656,597 of them applied to the AAPL book.

Latency is the wall time of one book update: two `steady_clock` reads around the call, so about 36 ns of clock overhead is included in every sample. Median of 5 runs, process pinned to logical CPU 3, input read from an already-decompressed file.

| Percentile | Latency (ns) | Range over 5 runs (ns) |
|---|---|---|
| p50 | 124 | 118 - 126 |
| p90 | 357 | 331 - 399 |
| p99 | 930 | 870 - 1,159 |
| p99.9 | 4,217 | 4,103 - 5,506 |

p99.99 and max are not reported: they swing by 3x between runs and are dominated by OS scheduling noise on Windows/WSL2.

**Setup:** Intel Core i5-12450H (12 logical CPUs), 7.6 GiB RAM visible to WSL2, Windows host running WSL2 Ubuntu (kernel 6.18.40.1-microsoft-standard-WSL2), GCC 15.2.0, CMake Release build with `-march=native`.

**Sanity checks on the full day:** 0 unknown order references, empty book at the close, and 552,947 BBO changes between 09:35 and 15:55 with 0 crossed quotes and a mean quoted spread of $0.0232 (averaged per event, not per unit of time).

**Reproduce:**

```bash
gzip -dk 01302019.NASDAQ_ITCH50.gz        # ~11 GB decompressed
for i in 1 2 3 4 5; do ./build/itch_sim --symbol AAPL --pin 3 01302019.NASDAQ_ITCH50 | grep "book update"; done
```

## Layout

```
include/itch/   types, parser, order_book, latency, pipeline
src/            order_book.cpp, latency.cpp, main.cpp
tools/          gen_synthetic.cpp
tests/          test_main.cpp
.github/        CI (Linux, macOS, Windows)
```
