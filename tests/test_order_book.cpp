// Unit tests for OrderBook in isolation, i.e. its container invariants
// (price ordering, FIFO within a level, O(1)-lookup cancellation) without
// any matching policy involved. Matching behavior is covered separately in
// test_matching_engine.cpp.

#include <catch2/catch_test_macros.hpp>

#include "lob/order_book.hpp"
#include "test_helpers.hpp"

using namespace lob;

TEST_CASE("empty book reports no orders on either side", "[order_book]") {
    OrderBook book;
    REQUIRE_FALSE(book.has_orders(Side::Buy));
    REQUIRE_FALSE(book.has_orders(Side::Sell));
    REQUIRE(book.order_count() == 0);
    REQUIRE_FALSE(book.contains(1));
    REQUIRE_FALSE(book.cancel_order(1));
}

TEST_CASE("best_price picks the highest bid and the lowest ask", "[order_book]") {
    OrderBook book;
    book.insert_resting_order(make_order(1, Side::Buy, OrderType::Limit, 100, 5, 0));
    book.insert_resting_order(make_order(2, Side::Buy, OrderType::Limit, 105, 5, 1));
    book.insert_resting_order(make_order(3, Side::Buy, OrderType::Limit, 102, 5, 2));
    REQUIRE(book.best_price(Side::Buy) == 105);

    book.insert_resting_order(make_order(4, Side::Sell, OrderType::Limit, 200, 5, 3));
    book.insert_resting_order(make_order(5, Side::Sell, OrderType::Limit, 198, 5, 4));
    book.insert_resting_order(make_order(6, Side::Sell, OrderType::Limit, 199, 5, 5));
    REQUIRE(book.best_price(Side::Sell) == 198);
}

TEST_CASE("orders at the same price level are FIFO by arrival order", "[order_book]") {
    OrderBook book;
    book.insert_resting_order(make_order(1, Side::Buy, OrderType::Limit, 100, 5, 0));
    book.insert_resting_order(make_order(2, Side::Buy, OrderType::Limit, 100, 5, 1));
    book.insert_resting_order(make_order(3, Side::Buy, OrderType::Limit, 100, 5, 2));

    REQUIRE(book.front_order(Side::Buy).id == 1);
    book.reduce_front_order(Side::Buy, 5);  // fully consume order 1
    REQUIRE(book.front_order(Side::Buy).id == 2);
    book.reduce_front_order(Side::Buy, 5);
    REQUIRE(book.front_order(Side::Buy).id == 3);
}

TEST_CASE("reduce_front_order partially fills without removing the order", "[order_book]") {
    OrderBook book;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 10, 0));
    book.reduce_front_order(Side::Sell, 4);

    REQUIRE(book.contains(1));
    REQUIRE(book.front_order(Side::Sell).quantity == 6);
    REQUIRE(book.front_order(Side::Sell).id == 1);
}

TEST_CASE("reduce_front_order to zero erases the order and, if last, the price level", "[order_book]") {
    OrderBook book;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 5, 0));
    book.insert_resting_order(make_order(2, Side::Sell, OrderType::Limit, 101, 5, 1));

    book.reduce_front_order(Side::Sell, 5);
    REQUIRE_FALSE(book.contains(1));
    REQUIRE(book.has_orders(Side::Sell));
    REQUIRE(book.best_price(Side::Sell) == 101);  // level 100 was erased, not left empty

    book.reduce_front_order(Side::Sell, 5);
    REQUIRE_FALSE(book.has_orders(Side::Sell));
    REQUIRE(book.order_count() == 0);
}

TEST_CASE("cancel_order removes an order from the middle of its queue without disturbing others", "[order_book]") {
    OrderBook book;
    book.insert_resting_order(make_order(1, Side::Buy, OrderType::Limit, 100, 5, 0));
    book.insert_resting_order(make_order(2, Side::Buy, OrderType::Limit, 100, 5, 1));
    book.insert_resting_order(make_order(3, Side::Buy, OrderType::Limit, 100, 5, 2));

    // Cancel the middle order. This exercises std::list's "erasing one
    // element doesn't invalidate iterators to other elements" guarantee --
    // orders 1 and 3's stored iterators must still be valid afterwards.
    REQUIRE(book.cancel_order(2));
    REQUIRE_FALSE(book.contains(2));
    REQUIRE(book.front_order(Side::Buy).id == 1);  // order 1 unaffected, still first

    book.reduce_front_order(Side::Buy, 5);  // consume order 1
    REQUIRE(book.front_order(Side::Buy).id == 3);  // order 3 unaffected, next in line
}

TEST_CASE("cancelling a nonexistent or already-removed order fails cleanly", "[order_book]") {
    OrderBook book;
    REQUIRE_FALSE(book.cancel_order(42));  // never existed

    book.insert_resting_order(make_order(1, Side::Buy, OrderType::Limit, 100, 5, 0));
    REQUIRE(book.cancel_order(1));
    REQUIRE_FALSE(book.cancel_order(1));  // already cancelled -- cancelling twice must not throw or corrupt state
}

TEST_CASE("cancelling the only order at a price level erases the level", "[order_book]") {
    OrderBook book;
    book.insert_resting_order(make_order(1, Side::Sell, OrderType::Limit, 100, 5, 0));
    REQUIRE(book.cancel_order(1));
    REQUIRE_FALSE(book.has_orders(Side::Sell));
}
