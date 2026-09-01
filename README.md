# limit-order-book

A limit order book and matching engine in C++17: price-time priority
matching, limit and market orders, partial fills, cancellation by order ID.
The container-choice-by-container-choice explanation is in [DESIGN.md](DESIGN.md); measured
throughput numbers and an honest discussion of what's actually slow live in
[BENCHMARKS.md](BENCHMARKS.md).

## What's here

```
include/lob/
  types.hpp             Price/Quantity/OrderId as integer ticks, Side, OrderType
  order.hpp             Order value type
  trade.hpp             Trade (execution report) value type
  order_book.hpp         OrderBook: price levels + FIFO queues + O(1) cancel lookup
  matching_engine.hpp    MatchingEngine: price-time priority matching policy

src/
  order_book.cpp
  matching_engine.cpp
  main.cpp               CLI driver (see "Replaying orders" below)

tests/                    Catch2 unit tests (19 cases, run via ctest)
benchmarks/                throughput + naive-vs-real comparison benchmarks
examples/sample_orders.csv  a small worked example for the CLI
```

`OrderBook` is the data structure (price levels, FIFO queues, the
cancellation index) and knows nothing about when two orders should trade.
`MatchingEngine` is the matching policy and knows nothing about
`std::map`/`std::list` — it only calls `OrderBook`'s public methods. Why
it's split this way, and why each container was chosen, is written up in
[DESIGN.md](DESIGN.md), both as prose and as comments next to the code
itself.

## Building

Requires CMake 3.16+ and a C++17 compiler. Tests and benchmarks are on by
default (`LOB_BUILD_TESTS`, `LOB_BUILD_BENCHMARKS`); the test suite pulls
in [Catch2](https://github.com/catchorg/Catch2) v3.7.1 via CMake
`FetchContent` on first configure, so the first build needs network access
and takes noticeably longer than subsequent ones.

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build
```

For performance numbers, build a separate Release directory instead of
switching the same one back and forth (`CMAKE_BUILD_TYPE` defaults to
`Release` if you omit it, but being explicit avoids surprises):

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
```

See the note in `BENCHMARKS.md` about why Debug-build numbers are
meaningless. This was developed and tested with g++ 15.2 under WSL2 on
Windows; MSVC isn't specifically tested but there's nothing GCC/Clang-
specific in the code (the CMake files set `/W4` under MSVC instead of
`-Wall -Wextra -Wpedantic`, so it should just work).

To skip building tests or benchmarks:

```sh
cmake -S . -B build -DLOB_BUILD_TESTS=OFF -DLOB_BUILD_BENCHMARKS=OFF
```

## Running the tests

```sh
cd build
ctest --output-on-failure
```

Or run the test binary directly for Catch2's own reporter output:
`./build/tests/lob_tests`. Coverage: full fill, partial fill on both sides
of a trade, one incoming order sweeping several resting orders, cancelling
a resting order mid-queue, cancelling an order that's already fully filled
(must fail cleanly, not throw), a market order sweeping multiple price
levels, and empty-book / non-crossing edge cases. See `tests/` for the
actual cases.

## Running the benchmarks

```sh
./build-release/benchmarks/bench_throughput 1000000   # arg = instruction count, default 1M
./build-release/benchmarks/bench_naive_comparison      # no args
```

`bench_throughput` replays a generated mix of limit orders, market orders,
and cancellations and reports real instructions/sec measured with
`std::chrono`. `bench_naive_comparison` builds a second, deliberately naive
order book (one flat `std::vector`, linear-scan cancellation) and times
cancellation against it at increasing book sizes, next to the real
`OrderBook`, to make the O(n)-vs-O(1) argument in `DESIGN.md` concrete
instead of theoretical. Full output and discussion: [BENCHMARKS.md](BENCHMARKS.md).

## Replaying orders through the CLI

`lob_cli` reads a sequence of instructions from a CSV file (or stdin, if no
file is given) and prints every trade, cancellation result, and the final
top-of-book state:

```sh
./build/lob_cli examples/sample_orders.csv
```

File format, one instruction per line (blank lines and `#` comments
ignored):

```
NEW,<id>,<BUY|SELL>,<LIMIT|MARKET>,<price>,<quantity>
CANCEL,<id>
```

`<price>` is ignored for `MARKET` orders and can be left blank
(`NEW,3,BUY,MARKET,,8`). See `examples/sample_orders.csv` for a short
worked example — two resting sell orders, a limit buy that partially fills
and rests the remainder, a cancellation, and a market order that sweeps
both price levels.

You can also pipe orders in directly:

```sh
printf 'NEW,1,SELL,LIMIT,101,10\nNEW,2,BUY,LIMIT,101,4\n' | ./build/lob_cli
```

## Why integer prices, not `double`

`Price` and `Quantity` (`include/lob/types.hpp`) are `std::int64_t`, not
floating point. `std::map` orders price levels with `operator<`, and the
matching engine's crossing check is a direct price comparison — floating
point equality/ordering is the wrong tool for that (`0.1 + 0.2 != 0.3` in
IEEE 754 double). Real exchanges trade in integer ticks for the same
reason; this codebase does too, and never converts to a decimal price
anywhere in the matching path. More on this, and every other container/
algorithm decision, in [DESIGN.md](DESIGN.md).
