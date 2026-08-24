#pragma once

#include <cstdint>

// Fundamental type aliases and enums shared by every component of the order
// book (data structure, matching engine, CLI driver, tests, benchmarks).
//
// Keeping these in one header means a type can be widened later (e.g.
// Quantity from 32 to 64 bits) in a single place.
namespace lob {

// Prices are represented as integer ticks, NOT as floating point.
//
// Why: std::map<Price, ...> uses operator< to order price levels, and to
// decide whether an incoming order "crosses" the book (e.g. is this buy
// price >= that ask price?). Floating point comparisons are unreliable for
// this: 0.1 + 0.2 != 0.3 in IEEE 754 double, so two logically-equal prices
// computed by different paths could compare unequal and silently create two
// separate price levels instead of one, or a crossing check could miscompute
// right at a boundary. Real exchanges avoid this entirely by trading in
// integer ticks (e.g. cents, or 1/10000ths of a currency unit) and letting
// the UI layer format ticks back into a decimal price for display. We do the
// same here: a Price of 10050 might mean "$100.50" if the tick size is
// $0.01, but the order book itself never needs to know that -- it only ever
// compares integers.
using Price = std::int64_t;

// Quantity (shares/contracts/lots) is also an integer for the same reason:
// exact arithmetic. Partial fills repeatedly subtract from a resting order's
// remaining quantity, and floating point subtraction can accumulate error
// over many fills, eventually leaving a "ghost" quantity that's neither
// exactly zero nor a sensible remainder.
using Quantity = std::int64_t;

// Every order gets a unique, monotonically increasing ID assigned by the
// caller (the CLI driver / order source). Used as the key into the
// OrderBook's cancellation lookup table.
using OrderId = std::uint64_t;

// A logical "arrival sequence number", not a wall-clock timestamp.
//
// Price-time priority needs to know, among orders resting at the same price,
// which one arrived first. A monotonically increasing counter (0, 1, 2, ...)
// handed out by the caller as each order is submitted is simpler and more
// robust than reading the system clock: it can't go backwards, has no
// resolution limits, and never collides between two orders submitted in the
// same clock tick. The FIFO container itself (see order_book.hpp) is what
// actually enforces the ordering -- this field is mostly useful for
// diagnostics/tests/CSV replay, since the container's insertion order IS the
// time priority.
using Sequence = std::uint64_t;

// Buy ("bid") or sell ("ask") side of the book.
enum class Side {
    Buy,
    Sell,
};

// Limit orders rest on the book at a specified price if they don't fully
// fill immediately. Market orders execute immediately against the best
// available price(s) and any unfilled remainder is discarded, never rests.
enum class OrderType {
    Limit,
    Market,
};

// Flips Buy <-> Sell; used whenever code needs "the opposite side of the
// book" (e.g. an incoming buy order matches against the resting sell side).
constexpr Side opposite(Side side) {
    return side == Side::Buy ? Side::Sell : Side::Buy;
}

}  // namespace lob
