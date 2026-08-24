#include "lob/order_book.hpp"

#include <cassert>
#include <utility>

namespace lob {

bool OrderBook::has_orders(Side side) const {
    return side == Side::Buy ? !bids_.empty() : !asks_.empty();
}

Price OrderBook::best_price(Side side) const {
    assert(has_orders(side) && "best_price() requires at least one resting order on this side");
    return side == Side::Buy ? bids_.begin()->first : asks_.begin()->first;
}

const Order& OrderBook::front_order(Side side) const {
    assert(has_orders(side) && "front_order() requires at least one resting order on this side");
    // begin() is the best price level (highest bid / lowest ask, thanks to
    // the comparators on BidMap/AskMap), and within that level, the front of
    // the list is the order that has been waiting longest -- i.e. exactly
    // the order price-time priority says should fill next.
    return side == Side::Buy ? bids_.begin()->second.front() : asks_.begin()->second.front();
}

void OrderBook::reduce_front_order(Side side, Quantity fill_quantity) {
    assert(has_orders(side) && "reduce_front_order() requires at least one resting order on this side");

    Order* front = side == Side::Buy ? &bids_.begin()->second.front() : &asks_.begin()->second.front();
    assert(fill_quantity > 0 && fill_quantity <= front->quantity && "fill_quantity out of range");

    front->quantity -= fill_quantity;
    if (front->quantity == 0) {
        // Fully consumed: remove it from the book entirely. Copy the
        // location out of `locations_` first, since erase_order() below
        // erases that very map entry -- holding a reference to it across
        // the erase would dangle.
        OrderId id = front->id;
        OrderLocation location = locations_.at(id);
        erase_order(id, location);
    }
}

void OrderBook::insert_resting_order(const Order& order) {
    assert(order.quantity > 0 && "cannot insert a resting order with non-positive quantity");
    assert(!contains(order.id) && "order id already present on the book");

    if (order.side == Side::Buy) {
        // operator[] default-constructs an empty PriceLevel the first time
        // a given price is seen, or returns the existing one -- either way
        // we get a queue to push_back onto.
        PriceLevel& level = bids_[order.price];
        level.push_back(order);
        locations_.emplace(order.id, OrderLocation{Side::Buy, order.price, std::prev(level.end())});
    } else {
        PriceLevel& level = asks_[order.price];
        level.push_back(order);
        locations_.emplace(order.id, OrderLocation{Side::Sell, order.price, std::prev(level.end())});
    }
}

bool OrderBook::cancel_order(OrderId order_id) {
    auto it = locations_.find(order_id);
    if (it == locations_.end()) {
        // Not on the book: already filled, already cancelled, or never
        // existed. Fail cleanly rather than throwing -- a caller trying to
        // cancel an order that just finished filling in a race is a normal,
        // expected occurrence, not an exceptional one.
        return false;
    }
    // Copy before erasing, same reasoning as in reduce_front_order().
    OrderLocation location = it->second;
    erase_order(order_id, location);
    return true;
}

bool OrderBook::contains(OrderId order_id) const {
    return locations_.find(order_id) != locations_.end();
}

std::size_t OrderBook::order_count() const {
    return locations_.size();
}

void OrderBook::erase_order(OrderId order_id, const OrderLocation& location) {
    // std::list::erase on a specific iterator is O(1) and -- critically --
    // does not invalidate any other iterator into the list, including ones
    // stored in `locations_` for other orders at this same price level.
    // This is the whole reason PriceLevel is a std::list rather than a
    // std::deque or std::vector; see the comment on PriceLevel in
    // order_book.hpp.
    if (location.side == Side::Buy) {
        auto level_it = bids_.find(location.price);
        level_it->second.erase(location.iterator);
        if (level_it->second.empty()) {
            // An empty price level left in the map would be indistinguishable
            // from "no orders here" except by explicitly checking .empty(),
            // and worse, it would still show up as the best price at
            // begin() with nothing to actually match against. Erase it so
            // the map only ever contains price levels that have real
            // resting orders.
            bids_.erase(level_it);
        }
    } else {
        auto level_it = asks_.find(location.price);
        level_it->second.erase(location.iterator);
        if (level_it->second.empty()) {
            asks_.erase(level_it);
        }
    }
    locations_.erase(order_id);
}

}  // namespace lob
