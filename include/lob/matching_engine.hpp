#pragma once

#include <vector>

#include "lob/order.hpp"
#include "lob/order_book.hpp"
#include "lob/trade.hpp"

namespace lob {

// MatchingEngine implements price-time priority matching on top of an
// OrderBook. This is deliberately a separate class from OrderBook: OrderBook
// only knows how to store resting orders and answer "what's the best price"
// / "who's first in line" / "remove this order" -- it has no opinion about
// when two orders should trade. MatchingEngine is the thing that has that
// opinion. Keeping the split means the matching *policy* (should this
// incoming order cross the book, how far should it sweep, what happens to
// any leftover quantity) can be read, tested, and reasoned about without
// wading through container bookkeeping, and vice versa.
//
// MatchingEngine does not own an OrderBook -- it operates on one passed in
// by reference. That mirrors how a real matching engine is just logic
// applied to a book (or many books, one per instrument); the book's
// lifetime and identity belong to whoever owns the instrument, not to the
// algorithm that matches orders against it.
class MatchingEngine {
public:
    // Submits `incoming` (a limit or market order) against `book`.
    //
    // Behavior:
    //  - The incoming order matches against the OPPOSITE side of the book
    //    in strict price-time priority: best price first, and within a
    //    price level, the order that arrived first (front of the FIFO
    //    queue) first.
    //  - A limit order only matches while its price crosses the resting
    //    price (a buy at price P matches asks priced <= P; a sell at price
    //    P matches bids priced >= P). Once it no longer crosses, or the
    //    incoming quantity is exhausted, matching stops.
    //  - A market order matches at whatever price the book offers,
    //    regardless of price, until either its quantity is exhausted or the
    //    opposite side of the book runs out of orders.
    //  - If a limit order still has unfilled quantity after matching, the
    //    remainder rests on the book (inserted via
    //    OrderBook::insert_resting_order). A market order's unfilled
    //    remainder is discarded -- market orders never rest.
    //
    // Returns the list of trades produced, in the order they occurred (so
    // the first trade in the vector is against the order that had highest
    // time priority at the best price).
    std::vector<Trade> submit(OrderBook& book, Order incoming);

private:
    // True if `incoming` (at `incoming_price`, of type `incoming_type`)
    // is willing to trade against a resting order priced at `resting_price`
    // on `resting_side`. Market orders always cross; limit orders cross only
    // if their price doesn't lose to the resting price.
    static bool crosses(OrderType incoming_type, Side incoming_side, Price incoming_price, Price resting_price);
};

}  // namespace lob
