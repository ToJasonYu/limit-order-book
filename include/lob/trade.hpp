#pragma once

#include "lob/types.hpp"

namespace lob {

// A single execution report: one resting order and one incoming order
// crossed at `price` for `quantity`. A single incoming order that sweeps
// three resting orders produces three Trade records, one per resting order
// it touched -- this mirrors how real exchanges report fills (one execution
// per matched pair, not one aggregated blob per incoming order), and makes
// it possible to reconstruct exactly which resting orders were consumed and
// in what order.
struct Trade {
    OrderId resting_order_id{};
    OrderId incoming_order_id{};

    // Execution price. Always the resting order's price, never the
    // incoming order's: this is standard "the resting order sets the
    // price" behavior, and is what makes market orders (which have no
    // price of their own) executable at all -- they simply take whatever
    // price the resting side is offering.
    Price price{};

    Quantity quantity{};

    // Which side the *incoming* (aggressor) order was on. Combined with the
    // two order IDs above this fully describes the trade: e.g. Side::Buy
    // means incoming_order_id bought from resting_order_id.
    Side aggressor_side{Side::Buy};
};

}  // namespace lob
