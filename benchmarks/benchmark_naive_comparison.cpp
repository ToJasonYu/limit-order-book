// Honest before/after comparison for the one operation DESIGN.md argues
// most depends on container choice: cancellation. NaiveOrderBook below is
// the "obvious first attempt" -- every resting order in a single flat
// std::vector -- compared against the real lob::OrderBook (std::map of
// std::list, plus the unordered_map index) at increasing resting-order
// counts.
//
// Both books are pre-loaded with `n` resting orders (all at the same
// price, deliberately -- that's the scenario where the difference matters
// most: many orders competing at one busy price level), then the same
// fixed number of cancellations of randomly chosen existing order ids is
// timed on each. If the numbers below don't show naive degrading roughly
// linearly with `n` while lob::OrderBook stays roughly flat, something is
// wrong with either this benchmark or the implementation -- that's the
// concrete, falsifiable prediction DESIGN.md's Big-O argument makes.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

#include "lob/order_book.hpp"

using namespace lob;

namespace {

// The "obvious" implementation before reaching for std::map/std::list:
// one unsorted std::vector holding every resting order. Cancelling by ID
// means scanning for it (no index) and then erasing from the middle, which
// shifts every following element down by one -- both O(n).
class NaiveOrderBook {
public:
    void insert(const Order& order) { orders_.push_back(order); }

    bool cancel(OrderId id) {
        auto it = std::find_if(orders_.begin(), orders_.end(), [id](const Order& o) { return o.id == id; });
        if (it == orders_.end()) {
            return false;
        }
        orders_.erase(it);
        return true;
    }

private:
    std::vector<Order> orders_;
};

Order make_resting_order(OrderId id) {
    Order order;
    order.id = id;
    order.side = Side::Buy;
    order.type = OrderType::Limit;
    order.price = 100;  // same price for every order: worst case for the naive scan,
                         // and the realistic "busy price level" case for lob::OrderBook
    order.quantity = 1;
    order.original_quantity = 1;
    order.sequence = id;
    return order;
}

}  // namespace

int main() {
    const std::vector<std::size_t> sizes = {1'000, 5'000, 10'000, 25'000, 50'000, 100'000};
    const std::size_t cancels_per_size = 2'000;
    std::mt19937_64 rng(7);

    std::cout << "resting_orders,naive_us_total,naive_ns_per_cancel,lob_us_total,lob_ns_per_cancel,speedup\n";

    for (std::size_t n : sizes) {
        NaiveOrderBook naive;
        OrderBook book;
        std::vector<OrderId> ids(n);
        for (std::size_t i = 0; i < n; ++i) {
            const OrderId id = static_cast<OrderId>(i + 1);
            ids[i] = id;
            naive.insert(make_resting_order(id));
            book.insert_resting_order(make_resting_order(id));
        }

        std::vector<OrderId> cancel_ids = ids;
        std::shuffle(cancel_ids.begin(), cancel_ids.end(), rng);
        cancel_ids.resize(std::min(cancels_per_size, cancel_ids.size()));

        const auto t0 = std::chrono::steady_clock::now();
        for (OrderId id : cancel_ids) {
            naive.cancel(id);
        }
        const auto t1 = std::chrono::steady_clock::now();

        for (OrderId id : cancel_ids) {
            book.cancel_order(id);
        }
        const auto t2 = std::chrono::steady_clock::now();

        const double naive_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
        const double lob_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
        const double naive_ns_per = naive_us * 1000.0 / static_cast<double>(cancel_ids.size());
        const double lob_ns_per = lob_us * 1000.0 / static_cast<double>(cancel_ids.size());

        std::cout << n << "," << naive_us << "," << naive_ns_per << "," << lob_us << "," << lob_ns_per << ","
                  << (lob_us > 0.0 ? naive_us / lob_us : 0.0) << "\n";
    }

    return 0;
}
