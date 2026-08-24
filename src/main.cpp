// Command-line driver: replays a sequence of orders from a CSV file (or
// stdin, if no file is given) through a single OrderBook/MatchingEngine and
// prints the resulting trades and cancellation results as it goes.
//
// This is deliberately a thin layer: all it does is parse lines of text
// into Order structs, call MatchingEngine::submit() or
// OrderBook::cancel_order(), and print what happened. None of the order
// book logic lives here.
//
// CSV format, one instruction per line, blank lines and lines starting with
// '#' ignored:
//
//   NEW,<id>,<BUY|SELL>,<LIMIT|MARKET>,<price>,<quantity>
//   CANCEL,<id>
//
// price is ignored (may be left blank, e.g. "NEW,3,BUY,MARKET,,8") for
// MARKET orders. Example:
//
//   NEW,1,SELL,LIMIT,101,10
//   NEW,2,BUY,LIMIT,100,5
//   NEW,3,BUY,LIMIT,101,10
//   CANCEL,2
//   NEW,4,SELL,MARKET,,3

#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "lob/matching_engine.hpp"

namespace {

using lob::Order;
using lob::OrderType;
using lob::Price;
using lob::Quantity;
using lob::Side;

std::string trim(const std::string& s) {
    const auto begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return "";
    const auto end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
}

std::vector<std::string> split_csv_line(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream ss(line);
    std::string field;
    while (std::getline(ss, field, ',')) {
        fields.push_back(trim(field));
    }
    return fields;
}

// One parsed line of input: either a new order to submit, or a cancellation.
struct Instruction {
    bool is_cancel = false;
    Order order;         // valid when !is_cancel
    lob::OrderId cancel_id = 0;  // valid when is_cancel
};

std::optional<Instruction> parse_line(const std::string& raw_line, lob::Sequence sequence) {
    const std::string line = trim(raw_line);
    if (line.empty() || line[0] == '#') {
        return std::nullopt;
    }

    const std::vector<std::string> fields = split_csv_line(line);
    if (fields.empty()) {
        return std::nullopt;
    }

    if (fields[0] == "CANCEL") {
        if (fields.size() < 2) {
            std::cerr << "skipping malformed CANCEL line: " << raw_line << "\n";
            return std::nullopt;
        }
        Instruction instr;
        instr.is_cancel = true;
        instr.cancel_id = std::stoull(fields[1]);
        return instr;
    }

    if (fields[0] == "NEW") {
        if (fields.size() < 6) {
            std::cerr << "skipping malformed NEW line: " << raw_line << "\n";
            return std::nullopt;
        }
        Order order;
        order.id = std::stoull(fields[1]);
        order.side = (fields[2] == "BUY") ? Side::Buy : Side::Sell;
        order.type = (fields[3] == "MARKET") ? OrderType::Market : OrderType::Limit;
        order.price = fields[4].empty() ? Price{0} : static_cast<Price>(std::stoll(fields[4]));
        order.quantity = static_cast<Quantity>(std::stoll(fields[5]));
        order.original_quantity = order.quantity;
        order.sequence = sequence;

        Instruction instr;
        instr.is_cancel = false;
        instr.order = order;
        return instr;
    }

    std::cerr << "skipping unrecognized line: " << raw_line << "\n";
    return std::nullopt;
}

const char* side_name(Side side) {
    return side == Side::Buy ? "BUY" : "SELL";
}

void run(std::istream& input, std::ostream& out) {
    lob::OrderBook book;
    lob::MatchingEngine engine;

    std::string line;
    lob::Sequence sequence = 0;
    while (std::getline(input, line)) {
        const auto instruction = parse_line(line, sequence);
        ++sequence;
        if (!instruction) {
            continue;
        }

        if (instruction->is_cancel) {
            const bool ok = book.cancel_order(instruction->cancel_id);
            out << "CANCEL id=" << instruction->cancel_id << (ok ? " OK\n" : " FAILED (not on book)\n");
            continue;
        }

        const Order& incoming = instruction->order;
        const auto trades = engine.submit(book, incoming);
        for (const auto& trade : trades) {
            out << "TRADE resting=" << trade.resting_order_id << " incoming=" << trade.incoming_order_id
                << " price=" << trade.price << " qty=" << trade.quantity << " aggressor=" << side_name(trade.aggressor_side)
                << "\n";
        }
        if (trades.empty()) {
            out << "NEW id=" << incoming.id << " " << side_name(incoming.side) << " no immediate match\n";
        }
    }

    out << "--- final book ---\n";
    out << "bids: ";
    if (book.has_orders(Side::Buy)) {
        out << "best=" << book.best_price(Side::Buy);
    } else {
        out << "(empty)";
    }
    out << "  asks: ";
    if (book.has_orders(Side::Sell)) {
        out << "best=" << book.best_price(Side::Sell);
    } else {
        out << "(empty)";
    }
    out << "\nresting order count: " << book.order_count() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc > 2) {
        std::cerr << "usage: " << argv[0] << " [orders.csv]   (reads stdin if no file given)\n";
        return 1;
    }

    if (argc == 2) {
        std::ifstream file(argv[1]);
        if (!file) {
            std::cerr << "could not open file: " << argv[1] << "\n";
            return 1;
        }
        run(file, std::cout);
    } else {
        run(std::cin, std::cout);
    }

    return 0;
}
