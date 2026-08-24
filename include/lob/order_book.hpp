#pragma once

#include <list>
#include <map>
#include <unordered_map>

#include "lob/order.hpp"
#include "lob/types.hpp"

namespace lob {

// OrderBook is the data structure at the heart of the exchange: it stores
// every resting (unfilled, unmatched) order, grouped by side and price, in
// price-time priority order. It does NOT decide who trades with whom --
// that's MatchingEngine's job (see matching_engine.hpp). OrderBook only
// exposes the primitives needed to answer "what's the best price on this
// side?", "who's first in line at that price?", and "remove this order,
// whether because it filled or because it was cancelled."
//
// Splitting it this way means the container invariants (how do I find the
// best price, how do I keep FIFO order, how do I cancel in O(1)) live in
// exactly one place, and the matching *policy* (walk price levels while they
// cross, stop when quantity is exhausted, etc.) lives somewhere that doesn't
// need to know anything about std::map or std::list at all.
class OrderBook {
public:
    // ---- Price levels: why std::map, not std::unordered_map -------------
    //
    // A price level needs two things an unordered_map cannot give us:
    //
    //   1. Ordered iteration. To find the best bid/ask we need the
    //      minimum or maximum key. To let a market order (or a marketable
    //      limit order) sweep through multiple price levels, we need to
    //      walk the levels in price order, starting from the best and
    //      moving outward, stopping as soon as the order is filled or the
    //      price stops crossing. unordered_map has no notion of "next
    //      key in order" -- you'd have to scan every key and take a min/max
    //      each time, which is O(n) per lookup instead of O(1) amortized.
    //
    //   2. A stable "best price" that updates itself. std::map is a
    //      balanced binary search tree (red-black tree in every major
    //      implementation), so begin() is always the minimum key and it
    //      stays correct across insertions and erasures in O(log n) time.
    //      That's exactly "what's the best bid/ask right now" for free.
    //
    // The cost, versus unordered_map, is that individual operations are
    // O(log n) instead of O(1) average. For an order book, n is the number
    // of *distinct price levels* -- typically small (tens to low thousands)
    // even when the number of individual orders is huge, because many
    // orders share the same price. That makes the O(log n) cost negligible
    // in practice, and it buys us O(log n) best-price lookup and O(log n)
    // sequential sweeps across levels, which an unordered_map fundamentally
    // cannot offer at any cost.
    //
    // Bids are stored with a descending comparator (std::greater) so the
    // *highest* price -- the best bid, the price a buyer is most willing to
    // pay -- is at begin(). Asks use the default ascending order
    // (std::less) so the *lowest* price -- the best ask, the price a seller
    // is most willing to accept -- is at begin(). This lets both sides
    // share the same "best price is begin()" logic in the matching engine
    // instead of needing side-specific min/max handling.

    // ---- FIFO within a price level: why std::list, not std::deque -------
    //
    // Time priority means: among orders resting at the same price, the one
    // that arrived first must be filled first. That requires a FIFO
    // container, and the natural candidates are std::deque and std::list.
    //
    // We use std::list here, and it comes down to which operation needs to
    // be cheap: appending new orders at the back (both containers: O(1)),
    // or erasing an order that ISN'T at the front (this is where they
    // differ).
    //
    // Matching always consumes from the FRONT of the queue (oldest order
    // first) -- that part alone would make deque's O(1) pop_front just as
    // good as list's. The problem is cancellation: a resting order can be
    // cancelled at ANY position in the queue, not just the front, and
    // that's a first-class supported operation here, not an edge case.
    //
    // std::deque's iterator/reference stability guarantee only covers
    // insert/erase at the ends. Erasing from the *middle* of a deque
    // invalidates ALL of its iterators and references, not just the one
    // being erased (cppreference: "insertion or removal in the middle
    // invalidates all iterators and references"). That's fatal for our
    // design, because the cancellation lookup table below stores an
    // iterator into this container for every resting order -- if cancelling
    // ORDER A invalidates the stored iterator for ORDER B sitting three
    // slots away at the same price level, the next cancellation or fill
    // touching B is undefined behavior.
    //
    // std::list guarantees the opposite: erasing an element invalidates
    // ONLY the iterator to that element. Every other iterator into the list
    // -- including ones stored far away in the unordered_map below --
    // remains valid. That's precisely the guarantee an O(1)-cancel-from-
    // anywhere design needs. The price we pay is worse cache locality (list
    // nodes are individually heap-allocated and scattered in memory, vs.
    // deque's contiguous-ish chunked storage) and a heap allocation per
    // node. For an order book, where price levels rarely hold more than a
    // few dozen resting orders and cancellation correctness matters far
    // more than a few nanoseconds of cache-friendliness, that trade is the
    // right one. (A vector would be worse on both axes: erasing from the
    // middle is O(n) *and* invalidates iterators to every element after
    // the erased one, and reallocation on growth invalidates everything.)
    using PriceLevel = std::list<Order>;

    using BidMap = std::map<Price, PriceLevel, std::greater<Price>>;
    using AskMap = std::map<Price, PriceLevel, std::less<Price>>;

    OrderBook() = default;

    // True if there is at least one resting order on `side`.
    bool has_orders(Side side) const;

    // The best price on `side` (highest bid / lowest ask).
    // Precondition: has_orders(side) is true.
    Price best_price(Side side) const;

    // The order at the front of the best price level on `side` -- i.e. the
    // next order that will be filled if this side is matched against.
    // Precondition: has_orders(side) is true.
    const Order& front_order(Side side) const;

    // Reduces the quantity of the front order on `side` by `fill_quantity`
    // (which must be <= its current quantity). If the order's remaining
    // quantity reaches zero, it is fully consumed: erased from its price
    // level and from the cancellation lookup table. If that was the last
    // order at that price level, the price level itself is erased from the
    // map -- otherwise a price level with an empty queue would incorrectly
    // still show up as a valid (but hollow) best price.
    // Precondition: has_orders(side) is true.
    void reduce_front_order(Side side, Quantity fill_quantity);

    // Inserts `order` as a new resting order at the back of its price
    // level's queue (creating the price level if it doesn't exist yet), and
    // records its location in the cancellation lookup table. `order.id`
    // must not already be present in the book.
    void insert_resting_order(const Order& order);

    // ---- Cancellation: the problem it has to solve -----------------------
    //
    // Cancelling by ID means: given only an OrderId, find that order and
    // remove it from whatever price-level queue it's sitting in -- without
    // scanning every order on the book to find it. That requires an index
    // from OrderId directly to "where in the book is this", which is what
    // `locations_` below is for for: std::unordered_map<OrderId,
    // OrderLocation>, giving average O(1) lookup by ID.
    //
    // The lookup alone isn't enough, though -- once we've found the entry,
    // *removing* the order still has to be cheap. That's why OrderLocation
    // stores not just "which price level" but a std::list<Order>::iterator
    // pointing directly at the order's node within that price level's
    // queue. Combined with std::list's guarantee that erasing one element
    // never invalidates iterators to any other element (see the PriceLevel
    // comment above), this makes cancellation O(1) average: hash lookup to
    // find the iterator, list::erase on that iterator directly, no
    // scanning. If PriceLevel were a std::vector instead, this same
    // approach would be actively dangerous: erasing element i from a vector
    // shifts every element after it, silently invalidating every iterator
    // and stored index for those later orders -- the exact bug this design
    // is built to avoid.
    //
    // Returns false (without throwing) if `order_id` doesn't exist on the
    // book -- e.g. it already fully filled, was already cancelled, or was
    // never a valid ID. That's the "cancel an already-filled order should
    // fail cleanly" requirement: the caller gets an unambiguous boolean
    // instead of an exception or, worse, silently corrupting book state.
    bool cancel_order(OrderId order_id);

    // True if `order_id` currently refers to a resting order on the book.
    bool contains(OrderId order_id) const;

    // Number of resting orders currently on the book (both sides combined).
    std::size_t order_count() const;

    // Read-only access to the raw price-level maps, for the matching engine
    // to walk multiple levels (a market order sweeping the book) and for
    // tests/CLI reporting to print depth. Mutation always goes through the
    // methods above so the maps and `locations_` never drift out of sync
    // with each other.
    const BidMap& bids() const { return bids_; }
    const AskMap& asks() const { return asks_; }

private:
    // Where a resting order lives: which side/price-level map it's in, and
    // an iterator pointing directly at its node in that level's list. The
    // iterator is what makes cancel_order O(1) instead of a linear scan.
    struct OrderLocation {
        Side side;
        Price price;
        std::list<Order>::iterator iterator;
    };

    BidMap bids_;
    AskMap asks_;
    std::unordered_map<OrderId, OrderLocation> locations_;

    // Erases the order pointed to by `location` from its price level (and
    // erases the price level too if it becomes empty), and removes the
    // corresponding entry from `locations_`. Shared implementation behind
    // both reduce_front_order's "fully filled" path and cancel_order.
    void erase_order(OrderId order_id, const OrderLocation& location);
};

}  // namespace lob
