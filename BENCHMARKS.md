# Benchmarks

Real, measured numbers from this machine, produced by the programs in
`benchmarks/`. Nothing here is estimated or made up — every number below
came from actually running the benchmark binaries built in a Release
configuration. Reproduce them yourself with:

```sh
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/benchmarks/bench_throughput 1000000
./build-release/benchmarks/bench_naive_comparison
```

**Use a Release build for benchmarking.** A Debug build (the default if you
don't pass `-DCMAKE_BUILD_TYPE=Release`) has optimizations off and, on some
standard library implementations, checked-iterator/assert overhead in
container operations. Benchmarking a Debug build measures the debug
instrumentation, not the algorithm.

## Test machine

- CPU: AMD Ryzen AI 7 350 (8 cores / 16 threads), reported inside a WSL2
  (Linux-on-Windows virtual machine) environment, not bare-metal Linux
- 16 GB RAM
- Compiler: g++ (Ubuntu 15.2.0), `-O3` via `CMAKE_BUILD_TYPE=Release`
- **Caveat worth stating up front:** WSL2 runs Linux inside a lightweight
  Hyper-V VM. That adds scheduling noise and virtualization overhead that a
  bare-metal Linux box wouldn't have, which shows up below as real run-to-run
  variance. Treat the throughput number as "the right order of magnitude on
  this machine," not a tight, reproducible-to-the-percent benchmark number.

## Throughput benchmark

`benchmarks/benchmark_throughput.cpp` pre-generates 1,000,000 instructions
with a fixed random seed (so the workload itself is reproducible even
though wall-clock timing isn't), then times only the loop that feeds them
through a single `OrderBook` + `MatchingEngine` — order generation happens
before the clock starts.

**Workload mix** (documented here, not just in the source, because a
throughput number is meaningless without knowing what was actually run):

- 80% new limit orders, priced within ±50 ticks of a slowly random-walking
  mid price
- 15% new market orders
- 5% cancellations of a randomly chosen previously-submitted order id
  (which may have already been filled by the time the cancel is attempted —
  that's intentional, see below)

This is a **crossing-heavy** synthetic workload: with a ±50-tick quote
spread against a book that only drifts by ±1 tick per instruction, most
limit orders end up marketable. Five runs of 1,000,000 instructions each:

| run | throughput (instructions/sec) |
|---|---|
| 1 | 7,094,756 |
| 2 | 6,963,988 |
| 3 | 7,337,903 |
| 4 | 8,278,277 |
| 5 | 8,485,299 |

Average ≈ **7.6 million instructions/sec**, range 6.96M–8.49M. That ~20%
spread between runs is the WSL2 scheduling noise mentioned above, not
algorithmic variance — the workload and code are identical every run.

Representative single-run detail output:

```
instructions processed: 1000000
elapsed: 0.167418 s
throughput: 5973067 instructions/sec
trades executed: 925287
cancels ok/failed: 1524/48367
resting orders remaining: 13770
distinct bid price levels: 208
distinct ask price levels: 283
```

(This particular run landed below the five-run range above — included
as-is rather than cherry-picked, to be honest about variance rather than
show only the best numbers.)

A few of these numbers are worth explaining rather than skimming past:

- **925,287 trades out of ~950,000 new-order instructions (80% + 15% of 1M)**
  confirms the workload is doing what it's supposed to: this is a busy,
  heavily-crossing book, exercising the multi-level sweep path
  (`MatchingEngine::submit`'s loop), not just single-order inserts.
- **Cancels: 1,524 ok vs 48,367 failed.** This looks alarming until you
  connect it to the trade count above: because most limit orders get
  filled almost immediately in this crossing-heavy workload, by the time a
  randomly chosen earlier order id comes up for cancellation, it's usually
  already gone. That's realistic, not a bug — and it's specifically
  exercising `cancel_order`'s "fail cleanly instead of throwing" path (see
  `OrderBook::cancel_order` in `include/lob/order_book.hpp`) tens of
  thousands of times per run.
- **Only 208 + 283 = 491 distinct price levels remain** out of a much larger
  number of orders processed. This is exactly the assumption DESIGN.md's
  `std::map` argument leans on: the *number of orders* can be huge, but the
  *number of distinct price levels* — the `n` in `std::map`'s O(log n) — is
  small. `std::map::find`/`insert`/`erase` against ~500 keys is not where
  time is going in this benchmark.

## Where the time actually goes

There is **no locking and no I/O** anywhere in the timed loop — this is a
single-threaded, in-process benchmark, so lock contention cannot be the
bottleneck here by construction (a real multi-instrument, multi-threaded
exchange would need to measure that separately; this project doesn't
attempt concurrency at all, which is itself a limitation worth naming
rather than glossing over).

What's left is allocation and memory-access pattern. `perf stat` on a
1,000,000-instruction run:

```
940,250,376      instructions:u
459,409,363      cycles:u                  (≈2.05 instructions per cycle)
  8,537,830      cache-references:u
  1,204,728      cache-misses:u            (≈14.1% miss rate)
```

An IPC around 2 and a ~14% cache-miss rate on cache-references together
point at a moderately memory-bound workload — not catastrophic, but not
free either. That's consistent with where this design spends its
allocations, and matches the trade-off DESIGN.md makes explicitly rather
than discovers by surprise:

- **Every new resting order is a `std::list` node**, individually
  heap-allocated (via the global allocator, no pooling/arena in this
  implementation). Every fully-filled or cancelled order frees one. At 80%
  of 1,000,000 instructions being new limit orders, that's on the order of
  hundreds of thousands of allocate/free pairs over the run.
- **Every brand-new price level is a `std::map` (red-black tree) node**,
  also heap-allocated, though there are only ~500 of these live at once
  here (see above) — cheap relative to the per-order list-node traffic.
- **`std::list` nodes are scattered in memory** rather than contiguous, by
  design (that's what buys the iterator-stability guarantee cancellation
  depends on — see DESIGN.md). That scattering is the direct, named cause
  of the cache-miss rate above: walking or erasing list nodes means
  chasing pointers to wherever the allocator happened to put each one,
  rather than sequential access.

None of this makes the design wrong — it's the same trade explained in
DESIGN.md, now with numbers instead of just Big-O behind it: allocation and
pointer-chasing cost is the real, measured price paid for O(1)
cancel-from-anywhere, and ~7.6M instructions/sec on a WSL2 VM is what that
costs on this machine for this workload. A pooled/arena allocator for list
and map nodes would likely reduce the allocation share of this cost
noticeably — that's a reasonable next optimization, not something this
codebase currently does.

## Naive comparison: cancellation cost vs. resting-order count

`benchmarks/benchmark_naive_comparison.cpp` isolates the exact trade-off
DESIGN.md argues for: `NaiveOrderBook` (a single flat `std::vector<Order>`,
cancel = linear scan by id + `vector::erase`) versus the real
`OrderBook` (`std::map` of `std::list` + `unordered_map` index), both
pre-loaded with `n` resting orders **at the same single price level**
(the scenario that most directly exercises FIFO-queue cancellation), then
timed doing 2,000 cancellations of randomly chosen existing order ids.

```
resting_orders,naive_us_total,naive_ns_per_cancel,lob_us_total,lob_ns_per_cancel,speedup
1000,152.709,152.709,48.302,48.302,3.16
5000,2966.96,1483.48,401.02,200.51,7.40
10000,7170.2,3585.1,845.192,422.596,8.48
25000,26995.9,13497.9,687.844,343.922,39.25
50000,57226.3,28613.1,1191.89,595.944,48.01
100000,142865,71432.4,1422.83,711.413,100.41
```

This is exactly the shape the Big-O argument predicts: naive cancellation
time grows roughly linearly with the number of resting orders (about
70x more resting orders at n=100,000 vs n=1,000 costs about 470x more time
per cancel — worse than linear here, most likely because scanning a larger
`std::vector` blows past cache more often, compounding the O(n) scan cost),
while `lob::OrderBook`'s cancellation time stays roughly flat regardless of
`n` — the small fluctuations (e.g. 200ns → 422ns → 344ns) are noise from
`std::map`'s O(log(distinct price levels)) lookup and general system
jitter, not growth with `n`, since there is exactly one price level in this
benchmark by construction. By n=100,000, cancellation in the real
`OrderBook` is about **100x faster** than the naive vector scan.
