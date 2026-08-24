#pragma once

#include "lob/types.hpp"

namespace lob {

// A single order, either newly arrived or resting on the book.
//
// This struct is intentionally a plain value type with public fields: it
// gets copied into a std::list node when it rests on the book (see
// OrderBook), and mutated in place (quantity decreases) as it is partially
// filled. There's no behavior attached to it -- matching logic lives in
// MatchingEngine, not here -- so a struct is more honest than a class with
// getters/setters that would just forward to the fields anyway.
struct Order {
    OrderId id{};
    Side side{Side::Buy};
    OrderType type{OrderType::Limit};

    // Limit price in integer ticks. Unused (but still set to whatever the
    // caller passed, typically 0) for market orders -- a market order's
    // executable price is whatever the resting book offers, not a value the
    // incoming order specifies.
    Price price{};

    // Quantity still unfilled. Starts equal to the quantity the order was
    // submitted with and is decremented by MatchingEngine as fills occur.
    // When it reaches 0 the order is fully filled and removed from the book.
    Quantity quantity{};

    // Quantity the order was originally submitted with. Kept alongside
    // `quantity` (the remaining amount) so trade reports and tests can
    // express "how much of this order filled" as original - remaining,
    // without the caller having to track it separately.
    Quantity original_quantity{};

    // Arrival sequence number, used for FIFO tie-breaking diagnostics. See
    // Sequence in types.hpp for why this isn't a wall-clock timestamp.
    Sequence sequence{};
};

}  // namespace lob
