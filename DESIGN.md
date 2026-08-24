# Design

This document walks through every container choice and algorithmic decision
in this order book, the way I'd want to explain it in an interview. If
you're studying this codebase, read this alongside `include/lob/order_book.hpp`
and `include/lob/matching_engine.hpp` — the same reasoning also lives there
as comments, right next to the code it explains, but this document connects
the pieces into one narrative.

## The two-layer split: `OrderBook` vs `MatchingEngine`

`OrderBook` (`include/lob/order_book.hpp`, `src/order_book.cpp`) is pure data
structure: it stores resting orders in price-time priority and exposes a
small set of primitives — "what's the best price on this side?", "who's
first in line there?", "remove this order." It has no opinion about *when*
two orders should trade.

`MatchingEngine` (`include/lob/matching_engine.hpp`, `src/matching_engine.cpp`)
is pure policy: given an incoming order and a book, it decides whether and
how far to match, using only `OrderBook`'s public primitives. It never
touches `std::map` or `std::list` directly.

Why split it this way, instead of one `OrderBook::add_order()` that does
everything? Two reasons:

1. **Separation of concerns you can actually explain.** "How do I find the
   best price in O(log n)" and "when does an order cross the book" are
   different questions with different failure modes. Mixing them means a
   bug in one is harder to isolate from the other.
2. **The matching engine is reusable policy.** Nothing about `submit()`
   depends on `OrderBook`'s internals — it would work unchanged against any
   type that implements the same five-method interface. That's not a
   requirement here, but it's a sign the boundary is in the right place.

## Why `std::map` for price levels, not `std::unordered_map`

A price level needs two things `unordered_map` cannot give:

**Ordered iteration.** Finding the best bid/ask means finding the
maximum/minimum key. Sweeping a market order across multiple price levels
means visiting keys *in order*, starting from the best, stopping as soon as
price stops crossing. `unordered_map` has no concept of "the next key in
order" — you'd need to scan every key and take a min/max each time, turning
an O(1) "what's the best price" question into an O(n) one.

**A self-maintaining best price.** `std::map` is a balanced binary search
tree (red-black tree, in every major standard library implementation), so
`begin()` is always the minimum key, and stays correct across insertions and
deletions in O(log n). That's "what's the best bid/ask right now" for free,
every time the book changes.

The cost is O(log n) per operation instead of `unordered_map`'s O(1)
average. But *n* here is the number of **distinct price levels**, not the
number of orders — real books have orders clustered at round numbers and
tick boundaries, so a book with tens of thousands of resting orders might
have only hundreds of distinct prices. O(log n) against that small n is
negligible, and it buys ordered traversal that `unordered_map` cannot
provide at any price.

**The comparator trick:** bids use `std::greater<Price>` (descending), asks
use the default `std::less<Price>` (ascending). That means `begin()` is
*always* the best price on either side — the highest bid, or the lowest ask
— without the matching engine needing side-specific "is this a max-heap or
min-heap question" branching. `MatchingEngine::submit()` calls
`book.best_price(side)` and `book.front_order(side)` the same way regardless
of which side it's matching against.

## Why `std::list` for the FIFO queue at each price level, not `std::deque`

Time priority means: among orders resting at the same price, the one that
arrived first fills first. That needs a FIFO container, and appending to
the back is O(1) for both `std::deque` and `std::list` — so on the "add a
new resting order" and "consume from the front on a match" operations
alone, they're equivalent.

The decision comes down to **cancellation**, which is a required, first-class
operation here, not an edge case: an order can be cancelled at *any*
position in its queue, not just the front (whatever arrived first is still
resting if nothing has matched against it yet, but an order in the middle
of the queue — arrived second, third, tenth — is just as cancellable).

This is where `std::deque` breaks down. Its iterator-stability guarantee
only covers insertion/removal *at the ends*. Per
[cppreference](https://en.cppreference.com/w/cpp/container/deque): erasing
from the middle of a deque invalidates **all** of its iterators and
references, not just the one being erased. That's fatal for this design,
because the cancellation lookup table (below) stores an iterator into this
exact container for *every* resting order. If cancelling order A
invalidates the stored iterator for order B three slots away at the same
price level, the next operation touching B is undefined behavior — not a
crash you can reliably catch in testing, but silent corruption.

`std::list` gives the opposite guarantee: erasing an element invalidates
**only** the iterator to that element. Every other iterator into the list —
including ones stored far away in the cancellation table — stays valid.
That is exactly the guarantee an "O(1) cancel from anywhere" design needs.

The price: `std::list` nodes are individually heap-allocated and scattered
in memory, so it has worse cache locality than `std::deque`'s chunked
contiguous storage, and each `push_back` is a heap allocation rather than
amortized-O(1) into existing contiguous storage. For a data structure where
individual price levels typically hold a modest number of resting orders
and correctness of cancellation matters far more than shaving nanoseconds
off cache misses, that trade is the right one.

(`std::vector` would be strictly worse on both axes: erasing from the middle
is O(n) *and* invalidates every iterator to elements after the erased one,
and any reallocation on growth invalidates literally everything.)

## The cancellation problem, precisely

Cancelling by ID means: given only an `OrderId`, find that order and remove
it from wherever it's sitting — without scanning every order on the book.
That needs an index from ID straight to location, which is
`std::unordered_map<OrderId, OrderLocation>` (`locations_` in
`OrderBook`), giving average O(1) lookup.

But the lookup alone isn't the whole story — once found, *removing* the
order has to be cheap too, or the "O(1) cancellation" claim collapses back
into an O(n) scan at the removal step. That's why each `OrderLocation`
stores not just "which side and price" but a `std::list<Order>::iterator`
pointing directly at that order's node:

```cpp
struct OrderLocation {
    Side side;
    Price price;
    std::list<Order>::iterator iterator;
};
```

Cancellation becomes: hash lookup (`locations_.find(id)`, O(1) average) to
get the `OrderLocation`, `map.find(price)` (O(log n), to find the price
level) to get the level, then `list.erase(iterator)` directly on the stored
iterator — no scanning the queue to find the order. If the price level
becomes empty as a result, it's erased from the `std::map` too, so the map
never accumulates hollow price levels that would otherwise show up as a
"best price" with nothing actually resting there.

**What breaks with the wrong container:** if `PriceLevel` were a
`std::vector<Order>` instead, this exact same approach — store an iterator
(or index) per order, erase directly — would be actively dangerous. Erasing
element *i* shifts every element after it down by one, which both costs
O(n) *and* silently invalidates every iterator/index stored for those later
orders. The bug wouldn't show up as a crash on the cancellation that caused
it — it would show up later, as a *different* order's cancellation or fill
touching the wrong queue position, or as undefined behavior on a dangling
iterator. This is precisely the failure mode `std::list`'s stability
guarantee rules out by construction, which is the whole reason it's the
container used here despite the cache-locality cost.

## Partial fill accounting, step by step

`MatchingEngine::submit()` (`src/matching_engine.cpp`) runs one loop:

```cpp
while (incoming.quantity > 0 && book.has_orders(resting_side)) {
    if (!crosses(...)) break;
    const Order& resting = book.front_order(resting_side);
    const Quantity fill_quantity = std::min(incoming.quantity, resting.quantity);
    // record a Trade for fill_quantity ...
    incoming.quantity -= fill_quantity;
    book.reduce_front_order(resting_side, fill_quantity);
}
```

`fill_quantity = min(incoming.quantity, resting.quantity)` is the entire
partial-fill algorithm. There are exactly three cases, and the code doesn't
distinguish between them — they fall out of the same formula:

- **`incoming.quantity < resting.quantity`:** the incoming order is fully
  consumed (this trade fills all of it) and the loop exits (or moves to
  next price level if incoming was itself a multi-fill in progress, but here
  quantity is now 0 so the loop condition stops it). The resting order's
  quantity is reduced but stays > 0 — `OrderBook::reduce_front_order`
  decrements it in place and leaves it exactly where it was in the queue
  (still at the front, still earliest arrival). Its time priority is
  unaffected by having been partially filled — this is standard price-time
  priority behavior, not a shortcut: an order that's partially filled does
  not lose its place in line.
- **`incoming.quantity > resting.quantity`:** the resting order is fully
  consumed. `reduce_front_order` sees quantity hit 0 and removes it: erased
  from its `std::list` (O(1), via the stored iterator — see above), and if
  that was the last order at that price level, the level itself is erased
  from the `std::map` (so the book doesn't retain hollow price levels). The
  incoming order continues the loop with reduced quantity, now matching
  against whatever is next in line — the next order at this price level, or
  the next price level entirely if this one is now empty.
- **`incoming.quantity == resting.quantity`:** both are fully consumed. This
  is the "full fill" case, and it's not special-cased anywhere — it's just
  what the formula above produces when the two quantities happen to be
  equal.

After the loop, any quantity still remaining on `incoming`:

- If `incoming` was a **limit order**, the remainder is inserted as a new
  resting order via `OrderBook::insert_resting_order` — it goes to the back
  of the queue at its own price (a brand new arrival at that price, with the
  lowest time priority among orders resting there, even though the order
  itself may have partially executed already).
- If `incoming` was a **market order**, the remainder is simply discarded.
  Market orders execute against whatever liquidity exists right now and
  never rest waiting for more to show up — that's the defining property
  that distinguishes them from limit orders.

## Why prices and quantities are integers, not `double`

`std::map<Price, ...>` orders price levels using `operator<`, and the
crossing check (`incoming buy price >= resting ask price?`) is a direct
comparison. Floating point comparison is the wrong tool for this: `0.1 +
0.2 != 0.3` in IEEE 754 double, so two logically-identical prices computed
by different paths could compare unequal and silently create two separate
price levels for what should be one price, or a crossing check could
misfire right at a boundary. Quantities have the same problem across
repeated partial-fill subtraction: floating point error can accumulate over
many fills into a "ghost" remainder that's neither exactly zero nor a
sensible number.

Real exchanges sidestep this by trading in integer ticks (e.g. price in
cents, or in 1/10000ths of a unit) and letting a display/formatting layer
turn ticks back into a decimal price for humans. This codebase does the
same: `Price` and `Quantity` are `std::int64_t` (see `include/lob/types.hpp`).
The order book itself never needs to know what a tick is worth in dollars —
it only ever compares and subtracts exact integers.

## Testing and benchmarking approach

See `tests/` for the Catch2 suite (why Catch2 specifically is explained at
the top of `tests/CMakeLists.txt`) and `BENCHMARKS.md` for real, measured
throughput numbers and an honest discussion of what's actually limiting
them.
