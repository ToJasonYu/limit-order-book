#pragma once

#include "lob/order.hpp"
#include "lob/types.hpp"

// Small helper shared by the test files so each TEST_CASE can build an
// Order in one line instead of repeating every field. Not part of the
// library itself -- test-only.
inline lob::Order make_order(lob::OrderId id, lob::Side side, lob::OrderType type, lob::Price price,
                              lob::Quantity quantity, lob::Sequence sequence) {
    lob::Order order;
    order.id = id;
    order.side = side;
    order.type = type;
    order.price = price;
    order.quantity = quantity;
    order.original_quantity = quantity;
    order.sequence = sequence;
    return order;
}
