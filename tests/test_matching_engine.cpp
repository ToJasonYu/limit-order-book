// Unit tests for MatchingEngine: the price-time priority matching policy
// applied on top of OrderBook. Covers every scenario called out as required:
// full fill, partial fill, one incoming order consuming several resting
// orders, cancellation interacting with matching, market orders sweeping
// multiple price levels, and empty-book edge cases.

#include <catch2/catch_test_macros.hpp>

#include "lob/matching_engine.hpp"
#include "test_helpers.hpp"

using namespace lob;

TEST_CASE("full fill: incoming order exactly matches one resting order", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 10, 0));

    auto trades = engine.submit(book, make_order(2, Side::Buy, OrderType::Limit, 100, 10, 1));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].resting_order_id == 1);
    CHECK(trades[0].incoming_order_id == 2);
    CHECK(trades[0].price == 100);
    CHECK(trades[0].quantity == 10);
    CHECK(trades[0].aggressor_side == Side::Buy);

    CHECK_FALSE(book.has_orders(Side::Sell));  // resting order fully consumed
    CHECK_FALSE(book.has_orders(Side::Buy));   // incoming fully filled, never rested
}

TEST_CASE("partial fill: incoming smaller than resting order", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 10, 0));

    auto trades = engine.submit(book, make_order(2, Side::Buy, OrderType::Limit, 100, 4, 1));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == 4);
    CHECK(book.contains(1));
    CHECK(book.front_order(Side::Sell).quantity == 6);  // reduced in place
    CHECK(book.front_order(Side::Sell).id == 1);        // still first in line
    CHECK_FALSE(book.contains(2));                      // incoming fully filled
}

TEST_CASE("partial fill: incoming larger than resting order rests the remainder", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 6, 0));

    auto trades = engine.submit(book, make_order(2, Side::Buy, OrderType::Limit, 100, 9, 1));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == 6);
    CHECK_FALSE(book.contains(1));             // resting order fully consumed and erased
    CHECK_FALSE(book.has_orders(Side::Sell));  // and its price level with it
    REQUIRE(book.contains(2));
    CHECK(book.front_order(Side::Buy).quantity == 3);  // 9 - 6 rests on the buy side
}

TEST_CASE("one incoming order fills several resting orders in time-priority order", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 4, 0));
    book.insert_resting_order(make_order(2, Side::Sell, OrderType::Limit, 100, 4, 1));
    book.insert_resting_order(make_order(3, Side::Sell, OrderType::Limit, 101, 4, 2));

    auto trades = engine.submit(book, make_order(4, Side::Buy, OrderType::Limit, 101, 8, 3));

    REQUIRE(trades.size() == 2);
    CHECK(trades[0].resting_order_id == 1);  // order 1 arrived before order 2
    CHECK(trades[0].quantity == 4);
    CHECK(trades[1].resting_order_id == 2);
    CHECK(trades[1].quantity == 4);

    REQUIRE(book.has_orders(Side::Sell));
    CHECK(book.front_order(Side::Sell).id == 3);  // untouched, level 100 fully drained
    CHECK(book.best_price(Side::Sell) == 101);
}

TEST_CASE("cancellation of a resting order removes it from future matches", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 5, 0));
    book.insert_resting_order(make_order(2, Side::Sell, OrderType::Limit, 100, 5, 1));

    REQUIRE(book.cancel_order(1));

    auto trades = engine.submit(book, make_order(3, Side::Buy, OrderType::Limit, 100, 5, 2));
    REQUIRE(trades.size() == 1);
    CHECK(trades[0].resting_order_id == 2);  // order 1 was cancelled, order 2 fills instead
}

TEST_CASE("cancelling an order that has already fully filled fails cleanly", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 5, 0));

    auto trades = engine.submit(book, make_order(2, Side::Buy, OrderType::Limit, 100, 5, 1));
    REQUIRE(trades.size() == 1);
    REQUIRE_FALSE(book.contains(1));

    CHECK_FALSE(book.cancel_order(1));  // must return false, not throw
    CHECK_FALSE(book.cancel_order(2));  // incoming order also never rested
}

TEST_CASE("market order sweeps multiple price levels", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 5, 0));
    book.insert_resting_order(make_order(2, Side::Sell, OrderType::Limit, 101, 5, 1));
    book.insert_resting_order(make_order(3, Side::Sell, OrderType::Limit, 102, 5, 2));

    auto trades = engine.submit(book, make_order(4, Side::Buy, OrderType::Market, 0, 12, 3));

    REQUIRE(trades.size() == 3);
    CHECK((trades[0].resting_order_id == 1 && trades[0].price == 100 && trades[0].quantity == 5));
    CHECK((trades[1].resting_order_id == 2 && trades[1].price == 101 && trades[1].quantity == 5));
    CHECK((trades[2].resting_order_id == 3 && trades[2].price == 102 && trades[2].quantity == 2));

    REQUIRE(book.contains(3));
    CHECK(book.front_order(Side::Sell).quantity == 3);  // 5 - 2 remains resting
    CHECK_FALSE(book.contains(4));                      // market order never rests
}

TEST_CASE("market order with unfilled remainder discards it instead of resting", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 3, 0));

    auto trades = engine.submit(book, make_order(2, Side::Buy, OrderType::Market, 0, 100, 1));

    REQUIRE(trades.size() == 1);
    CHECK(trades[0].quantity == 3);
    CHECK_FALSE(book.has_orders(Side::Sell));
    CHECK_FALSE(book.contains(2));  // 97 unfilled units discarded, not resting
    CHECK(book.order_count() == 0);
}

TEST_CASE("submitting a limit order against an empty book just rests it", "[matching]") {
    OrderBook book;
    MatchingEngine engine;

    auto trades = engine.submit(book, make_order(1, Side::Buy, OrderType::Limit, 100, 10, 0));

    CHECK(trades.empty());
    REQUIRE(book.contains(1));
    CHECK(book.best_price(Side::Buy) == 100);
}

TEST_CASE("submitting a market order against an empty book does nothing", "[matching]") {
    OrderBook book;
    MatchingEngine engine;

    auto trades = engine.submit(book, make_order(1, Side::Sell, OrderType::Market, 0, 10, 0));

    CHECK(trades.empty());
    CHECK_FALSE(book.has_orders(Side::Sell));
    CHECK(book.order_count() == 0);
}

TEST_CASE("a limit order that doesn't cross the book rests without matching", "[matching]") {
    OrderBook book;
    MatchingEngine engine;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 105, 5, 0));

    // buy at 100 doesn't cross a resting ask at 105
    auto trades = engine.submit(book, make_order(2, Side::Buy, OrderType::Limit, 100, 5, 1));

    CHECK(trades.empty());
    CHECK(book.contains(1));
    CHECK(book.contains(2));
    CHECK(book.best_price(Side::Buy) == 100);
    CHECK(book.best_price(Side::Sell) == 105);
}
