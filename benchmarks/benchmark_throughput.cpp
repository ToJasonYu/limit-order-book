// Real throughput benchmark: generates a stream of random instructions
// (a mix of limit orders, market orders, and cancellations) up front, then
// measures how long the actual OrderBook + MatchingEngine take to process
// that exact stream, using std::chrono::steady_clock. Order generation is
// timed separately from processing so the reported number reflects only
// the engine, not the random number generator.
//
// This is a single-threaded, in-process benchmark: there is no locking and
// no I/O in the timed loop, so if the result is slower than you'd expect,
// the explanation has to be about the data structures themselves
// (allocation patterns, cache behavior, comparator/hash cost) -- see
// BENCHMARKS.md for that discussion, produced from this program's actual
// output, not a guessed number.

#include <chrono>
#include <cstdint>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include "lob/matching_engine.hpp"

using namespace lob;

namespace {

enum class InstrType { NewLimit, NewMarket, Cancel };

struct Instruction {
    InstrType type;
    OrderId id;     // new order's id, or the id to cancel
    Side side;
    Price price;    // meaningful only for NewLimit
    Quantity quantity;
};

// Builds `count` instructions with a fixed distribution:
//   80% new limit orders, quoted near a slowly random-walking mid price
//       (so a healthy fraction of them cross the book and produce trades,
//       rather than just piling up one-sided depth)
//   15% new market orders (guarantees repeated sweeps through the book)
//    5% cancellations of a randomly chosen previously-submitted limit
//       order id (it may already have been filled by the time it's
//       "cancelled" -- that's realistic and exercises the clean-failure
//       path of cancel_order() too)
// A fixed RNG seed makes this reproducible run to run.
std::vector<Instruction> generate_instructions(std::size_t count) {
    std::mt19937_64 rng(42);
    std::uniform_int_distribution<int> type_roll(0, 99);
    std::uniform_int_distribution<int> side_roll(0, 1);
    std::uniform_int_distribution<int> qty_roll(1, 100);
    std::uniform_int_distribution<int> price_offset_roll(-50, 50);
    std::uniform_int_distribution<int> walk_roll(-1, 1);

    Price mid = 100'000;
    OrderId next_id = 1;
    std::vector<OrderId> resting_candidates;  // ids we might try to cancel later
    resting_candidates.reserve(count);

    std::vector<Instruction> instructions;
    instructions.reserve(count);

    for (std::size_t i = 0; i < count; ++i) {
        mid += walk_roll(rng);
        const Side side = side_roll(rng) == 0 ? Side::Buy : Side::Sell;
        const int roll = type_roll(rng);

        if (roll < 5 && !resting_candidates.empty()) {
            std::uniform_int_distribution<std::size_t> pick(0, resting_candidates.size() - 1);
            instructions.push_back(Instruction{InstrType::Cancel, resting_candidates[pick(rng)], side, 0, 0});
        } else if (roll < 20) {
            const OrderId id = next_id++;
            instructions.push_back(Instruction{InstrType::NewMarket, id, side, 0, qty_roll(rng)});
        } else {
            const OrderId id = next_id++;
            const Price price = mid + price_offset_roll(rng);
            instructions.push_back(Instruction{InstrType::NewLimit, id, side, price, qty_roll(rng)});
            resting_candidates.push_back(id);
        }
    }
    return instructions;
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t instruction_count = 1'000'000;
    if (argc > 1) {
        instruction_count = std::stoull(argv[1]);
    }

    const std::vector<Instruction> instructions = generate_instructions(instruction_count);

    OrderBook book;
    MatchingEngine engine;
    std::size_t trades_executed = 0;
    std::size_t cancels_ok = 0;
    std::size_t cancels_failed = 0;

    const auto start = std::chrono::steady_clock::now();
    for (const Instruction& instr : instructions) {
        if (instr.type == InstrType::Cancel) {
            if (book.cancel_order(instr.id)) {
                ++cancels_ok;
            } else {
                ++cancels_failed;
            }
            continue;
        }

        Order order;
        order.id = instr.id;
        order.side = instr.side;
        order.type = (instr.type == InstrType::NewMarket) ? OrderType::Market : OrderType::Limit;
        order.price = instr.price;
        order.quantity = instr.quantity;
        order.original_quantity = instr.quantity;
        order.sequence = instr.id;

        trades_executed += engine.submit(book, order).size();
    }
    const auto end = std::chrono::steady_clock::now();

    const double seconds = std::chrono::duration<double>(end - start).count();
    const double throughput = static_cast<double>(instruction_count) / seconds;

    std::cout << "instructions processed: " << instruction_count << "\n";
    std::cout << "elapsed: " << seconds << " s\n";
    std::cout << "throughput: " << static_cast<std::uint64_t>(throughput) << " instructions/sec\n";
    std::cout << "trades executed: " << trades_executed << "\n";
    std::cout << "cancels ok/failed: " << cancels_ok << "/" << cancels_failed << "\n";
    std::cout << "resting orders remaining: " << book.order_count() << "\n";
    std::cout << "distinct bid price levels: " << book.bids().size() << "\n";
    std::cout << "distinct ask price levels: " << book.asks().size() << "\n";

    return 0;
}
