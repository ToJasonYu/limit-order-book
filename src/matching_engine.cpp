#include "lob/matching_engine.hpp"

#include <algorithm>

namespace lob {

bool MatchingEngine::crosses(OrderType incoming_type, Side incoming_side, Price incoming_price, Price resting_price) {
    if (incoming_type == OrderType::Market) {
        // Market orders have no price of their own -- they're willing to
        // trade at whatever the book is offering, full stop.
        return true;
    }
    // A buy crosses a resting ask if the buyer is willing to pay at least
    // the ask's price. A sell crosses a resting bid if the seller is
    // willing to accept at most the bid's price.
    return incoming_side == Side::Buy ? incoming_price >= resting_price : incoming_price <= resting_price;
}

std::vector<Trade> MatchingEngine::submit(OrderBook& book, Order incoming) {
    std::vector<Trade> trades;
    const Side resting_side = opposite(incoming.side);

    // Walk the resting side one best-price-level at a time, and within each
    // level, one FIFO front-of-queue order at a time. This single loop is
    // what implements every required matching scenario: a full fill against
    // one resting order, a partial fill, one incoming order consuming
    // several resting orders in a row, and a market/marketable-limit order
    // sweeping across multiple price levels -- they're all just different
    // numbers of iterations of the same loop, not separate code paths.
    while (incoming.quantity > 0 && book.has_orders(resting_side)) {
        const Price resting_price = book.best_price(resting_side);
        if (!crosses(incoming.type, incoming.side, incoming.price, resting_price)) {
            // Best remaining price no longer crosses -- and since price
            // levels are visited in best-to-worst order, no level beyond
            // this one could cross either. Stop.
            break;
        }

        const Order& resting = book.front_order(resting_side);

        // The core of partial-fill accounting: this trade can only be as
        // large as whichever side has less left to give. If the incoming
        // order is smaller, it fully consumes itself against a resting
        // order that still has quantity left over (a partial fill of the
        // resting order, which keeps resting -- reduce_front_order handles
        // this by adjusting its quantity in place without removing it). If
        // the resting order is smaller, it is fully consumed and removed
        // from the book (reduce_front_order erases it, and erases the
        // price level too if that was the last order there), while the
        // incoming order continues on to match further down the book. If
        // they're equal, both are fully consumed -- "full fill" is simply
        // the fill_quantity == both quantities case of this same formula.
        const Quantity fill_quantity = std::min(incoming.quantity, resting.quantity);

        // Capture id/price before reduce_front_order(), which may fully
        // erase `resting` (invalidating the reference) if this fill
        // consumes it entirely.
        Trade trade;
        trade.resting_order_id = resting.id;
        trade.incoming_order_id = incoming.id;
        // Execution price is always the resting order's price: the order
        // that was already on the book sets the price the incoming
        // (aggressor) order must accept.
        trade.price = resting_price;
        trade.quantity = fill_quantity;
        trade.aggressor_side = incoming.side;
        trades.push_back(trade);

        incoming.quantity -= fill_quantity;
        book.reduce_front_order(resting_side, fill_quantity);
    }

    if (incoming.quantity > 0 && incoming.type == OrderType::Limit) {
        // Unfilled remainder of a limit order rests on the book at its own
        // price to await future matches.
        book.insert_resting_order(incoming);
    }
    // Unfilled remainder of a market order is simply discarded: market
    // orders execute against whatever liquidity exists right now and never
    // rest waiting for more.

    return trades;
}

}  // namespace lob
