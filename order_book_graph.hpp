#pragma once
#include "types.hpp"
#include "vertex.hpp"
#include <vector>
#include <memory>
#include <random>
#include <string>

namespace pregel {

class OrderVertex : public Vertex {
public:
    struct State {
        int64_t  limit_price;   
        uint64_t quantity;
        uint64_t filled_qty{0};
        Side     side;
        bool     active{true};
        uint32_t venue_id;
        uint64_t trader_id;
        uint64_t timestamp_ns;
    };

    OrderVertex(VertexId id, WorkerId owner, State state);
    void compute(std::span<const Message> inbox, Superstep step) override;

    const State& state() const { return state_; }

private:
    State    state_;
    int64_t  best_market_price_{0};
    uint32_t steps_without_fill_{0};
};

class PriceLevelVertex : public Vertex {
public:
    struct State {
        int64_t  price;
        Side     side;
        uint64_t total_qty{0};
        uint32_t order_count{0};
        uint32_t venue_id;
        bool     is_best_level{false};
    };

    PriceLevelVertex(VertexId id, WorkerId owner, State state);
    void compute(std::span<const Message> inbox, Superstep step) override;

    const State& state() const { return state_; }

private:
    State   state_;
    int64_t prev_total_qty_{0};
};

class VenueVertex : public Vertex {
public:
    struct State {
        uint32_t venue_id;
        std::string name;   // "NYSE", "NASDAQ", "CBOE", etc. tickers
        int64_t best_bid{0};
        int64_t best_ask{0};
        uint64_t bid_size{0};
        uint64_t ask_size{0};
        double  latency_us{5.0};    
        bool    is_primary{false};
    };

    VenueVertex(VertexId id, WorkerId owner, State state);
    void compute(std::span<const Message> inbox, Superstep step) override;

    const State& state() const { return state_; }

private:
    State   state_;
    uint32_t tick_{0};
    std::mt19937_64 rng_;
};

class MarketMakerVertex : public Vertex {
public:
    struct State {
        uint64_t mm_id;
        int64_t  net_position{0};      // positive = long
        int64_t  pnl_fixed{0};    // fixed-point cents
        uint64_t fill_count{0};
        uint64_t arb_signals_sent{0};
        double   inventory_limit{1e6};   // max abs position
    };

    MarketMakerVertex(VertexId id, WorkerId owner, State state);
    void compute(std::span<const Message> inbox, Superstep step) override;

    const State& state() const { return state_; }

private:
    State    state_;
    int64_t  last_known_bid_{0};
    int64_t  last_known_ask_{0};
};

struct OrderBookGraphParams {
    uint32_t venue_count        = 5;
    uint32_t price_levels       = 20; // per side per venue
    uint64_t orders_per_level   = 50;
    uint32_t market_maker_count = 10;
    uint32_t worker_count       = 4;
    uint64_t seed               = 42;
};

struct GraphData {
    std::vector<std::unique_ptr<Vertex>> vertices;
    uint64_t total_edges{0};
    uint32_t venue_count{0};
    uint32_t price_level_count{0};
    uint64_t order_count{0};
    uint32_t mm_count{0};
};

GraphData build_order_book_graph(const OrderBookGraphParams& params);

} 
