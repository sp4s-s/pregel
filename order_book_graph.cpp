#include "order_book_graph.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <sstream>
#include <numeric>

namespace pregel {

static constexpr int64_t FIXED_SCALE = 1'000'000LL;   // 1e6 fixed-point
static constexpr int64_t TICK_SIZE   = 100;             // $0.0001 in fixed-point
static constexpr int64_t ARB_THRESHOLD_BPS = 5;         // 0.5bps arb minimum


OrderVertex::OrderVertex(VertexId id, WorkerId owner, State state)
    : Vertex(id, VertexType::ORDER, owner), state_(state)
{}

void OrderVertex::compute(std::span<const Message> inbox, Superstep step) {
    if (!state_.active) {
        vote_to_halt();
        return;
    }

    for (const auto& msg : inbox) {
        switch (msg.type) {
        case MsgType::PRICE_UPDATE:
            best_market_price_ = (state_.side == Side::BID)
                                 ? msg.payload.price.bid
                                 : msg.payload.price.ask;
            break;
        case MsgType::ORDER_FILL:
            state_.filled_qty += msg.payload.fill.qty;
            if (state_.filled_qty >= state_.quantity) {
                state_.active = false;
                vote_to_halt();
                return;
            }
            break;
        case MsgType::CANCEL_ACK:
            state_.active = false;
            vote_to_halt();
            return;
        default: break;
        }
    }

    bool marketable = false;
    if (best_market_price_ > 0) {
        if (state_.side == Side::BID && best_market_price_ <= state_.limit_price)
            marketable = true;
        if (state_.side == Side::ASK && best_market_price_ >= state_.limit_price)
            marketable = true;
    }

    if (marketable) {
        for (const auto& edge : edges_) {
            Message fill{};
            fill.type = MsgType::ORDER_FILL;
            fill.step = step;
            fill.payload.fill.price    = state_.limit_price;
            fill.payload.fill.qty      = state_.quantity - state_.filled_qty;
            fill.payload.fill.trade_id = id_ ^ (step << 32);
            send_message(edge.dst, fill);
        }
        steps_without_fill_ = 0;
    } else {
        steps_without_fill_++;
        if (steps_without_fill_ > 10) {
            vote_to_halt();
        }
    }
}


PriceLevelVertex::PriceLevelVertex(VertexId id, WorkerId owner, State state)
    : Vertex(id, VertexType::PRICE_LEVEL, owner), state_(state)
{}

void PriceLevelVertex::compute(std::span<const Message> inbox, Superstep step) {
    for (const auto& msg : inbox) {
        if (msg.type == MsgType::ORDER_FILL) {
            uint64_t fill_qty = msg.payload.fill.qty;
            if (fill_qty >= state_.total_qty) {
                state_.total_qty   = 0;
                state_.order_count = 0;
            } else {
                state_.total_qty -= fill_qty;
                if (state_.order_count > 0) state_.order_count--;
            }
        } else if (msg.type == MsgType::PRICE_UPDATE) {
            for (const auto& edge : edges_) {
                if (edge.type == 1) { // edge to orders
                    Message upd = msg;
                    upd.step = step;
                    send_message(edge.dst, upd);
                }
            }
        }
    }

    if (state_.total_qty != prev_total_qty_) {
        Message upd{};
        upd.type = MsgType::PRICE_UPDATE;
        upd.step = step;
        upd.payload.price.bid     = (state_.side == Side::BID) ? state_.price : 0;
        upd.payload.price.ask     = (state_.side == Side::ASK) ? state_.price : 0;
        upd.payload.price.bid_qty = (state_.side == Side::BID) ? state_.total_qty : 0;
        upd.payload.price.ask_qty = (state_.side == Side::ASK) ? state_.total_qty : 0;

        for (const auto& edge : edges_) {
            if (edge.type == 2) { // edge to venue/market-maker
                send_message(edge.dst, upd);
            }
        }
        prev_total_qty_ = state_.total_qty;
    }

    if (state_.total_qty == 0 && inbox.empty()) {
        vote_to_halt();
    }
}


VenueVertex::VenueVertex(VertexId id, WorkerId owner, State state)
    : Vertex(id, VertexType::VENUE, owner)
    , state_(state)
    , rng_(state.venue_id * 0xDEADBEEF12345678ULL)
{}

void VenueVertex::compute(std::span<const Message> inbox, Superstep step) {
    for (const auto& msg : inbox) {
        if (msg.type == MsgType::PRICE_UPDATE) {
            if (msg.payload.price.bid > 0) {
                state_.best_bid  = std::max(state_.best_bid, msg.payload.price.bid);
                state_.bid_size += msg.payload.price.bid_qty;
            }
            if (msg.payload.price.ask > 0) {
                if (state_.best_ask == 0 || msg.payload.price.ask < state_.best_ask) {
                    state_.best_ask  = msg.payload.price.ask;
                    state_.ask_size += msg.payload.price.ask_qty;
                }
            }
        }
    }

    std::normal_distribution<double> noise(0.0, 2.0 * TICK_SIZE);
    int64_t mid = (state_.best_bid + state_.best_ask) / 2;
    if (mid == 0) mid = 100'000 * FIXED_SCALE; // $100 default
    mid += static_cast<int64_t>(noise(rng_));
    int64_t half_spread = TICK_SIZE * 2;
    state_.best_bid = mid - half_spread;
    state_.best_ask = mid + half_spread;
    tick_++;

    Message nbbo{};
    nbbo.type = MsgType::PRICE_UPDATE;
    nbbo.step = step;
    nbbo.payload.price.bid     = state_.best_bid;
    nbbo.payload.price.ask     = state_.best_ask;
    nbbo.payload.price.bid_qty = state_.bid_size;
    nbbo.payload.price.ask_qty = state_.ask_size;

    for (const auto& edge : edges_) {
        send_message(edge.dst, nbbo);
    }

}


MarketMakerVertex::MarketMakerVertex(VertexId id, WorkerId owner, State state)
    : Vertex(id, VertexType::MARKET_MAKER, owner), state_(state)
{}

void MarketMakerVertex::compute(std::span<const Message> inbox, Superstep step) {
    for (const auto& msg : inbox) {
        switch (msg.type) {
        case MsgType::ORDER_FILL: {
            auto& f = msg.payload.fill;
            int64_t delta_qty = static_cast<int64_t>(f.qty);
            state_.net_position += delta_qty;
            state_.fill_count++;
            state_.pnl_fixed -= f.price * delta_qty; // [simplified]
            break;
        }
        case MsgType::PRICE_UPDATE: {
            int64_t bid = msg.payload.price.bid;
            int64_t ask = msg.payload.price.ask;
            if (bid > 0) last_known_bid_ = bid;
            if (ask > 0) last_known_ask_ = ask;
            break;
        }
        default: break;
        }
    }

    if (last_known_bid_ > 0 && last_known_ask_ > 0) {
        int64_t spread = last_known_bid_ - last_known_ask_;
        if (spread > 0) {
            // Bid > Ask on different venues = arb opportunity
            int64_t mid   = (last_known_bid_ + last_known_ask_) / 2;
            int64_t bps   = (mid > 0) ? (spread * 10000 / mid) : 0;

            if (bps >= ARB_THRESHOLD_BPS) {
                Message arb{};
                arb.type = MsgType::ARBITRAGE_SIGNAL;
                arb.step = step;
                arb.payload.arb.spread  = bps;
                arb.payload.arb.venue_a = 0;
                arb.payload.arb.venue_b = 1;
                state_.arb_signals_sent++;

                for (const auto& edge : edges_) {
                    send_message(edge.dst, arb);
                }
            }
        }
    }
// Simple Strategy
    // Risk check: if over inventory limit, vote to halt
    if (std::abs(state_.net_position) > state_.inventory_limit) {
        vote_to_halt();
    } else if (inbox.empty() && outbox_.empty()) {
        vote_to_halt();
    }
}


GraphData build_order_book_graph(const OrderBookGraphParams& params) {
    GraphData data;
    std::mt19937_64 rng(params.seed);
    std::uniform_int_distribution<uint64_t> qty_dist(100, 10000);
    std::normal_distribution<double>        price_noise(0.0, 5.0);

    static constexpr int64_t BASE_PRICE = 150LL * FIXED_SCALE; // $150 stock

    VertexId next_id = 0;
    uint32_t w_count = params.worker_count;
    const std::vector<std::string> venue_names = {"NYSE","NASDAQ","CBOE","BATS","IEX"};

    std::vector<VertexId> venue_ids;
    for (uint32_t v = 0; v < params.venue_count; ++v) {
        VertexId vid = next_id++;
        venue_ids.push_back(vid);

        VenueVertex::State vs;
        vs.venue_id  = v;
        vs.name      = venue_names[v % venue_names.size()];
        vs.best_bid  = BASE_PRICE - 100 + static_cast<int64_t>(price_noise(rng));
        vs.best_ask  = BASE_PRICE + 100 + static_cast<int64_t>(price_noise(rng));
        vs.bid_size  = qty_dist(rng);
        vs.ask_size  = qty_dist(rng);
        vs.latency_us= 2.0 + v * 0.5;
        vs.is_primary= (v == 0);

        data.vertices.push_back(
            std::make_unique<VenueVertex>(vid, vid % w_count, vs));
        data.venue_count++;
    }

    std::vector<VertexId> price_level_ids;
    std::vector<VertexId> order_ids;

    for (uint32_t v = 0; v < params.venue_count; ++v) {
        VertexId venue_vid = venue_ids[v];

        for (uint32_t side_i = 0; side_i < 2; ++side_i) {
            Side side = (side_i == 0) ? Side::BID : Side::ASK;

            for (uint32_t lvl = 0; lvl < params.price_levels; ++lvl) {
                VertexId pl_id = next_id++;
                price_level_ids.push_back(pl_id);

                int64_t offset = (side == Side::BID)
                               ? -(static_cast<int64_t>(lvl + 1) * TICK_SIZE)
                               :  (static_cast<int64_t>(lvl + 1) * TICK_SIZE);
                int64_t price = BASE_PRICE + offset
                              + static_cast<int64_t>(price_noise(rng));

                PriceLevelVertex::State pls;
                pls.price       = price;
                pls.side        = side;
                pls.order_count = static_cast<uint32_t>(params.orders_per_level);
                pls.total_qty   = qty_dist(rng) * params.orders_per_level;
                pls.venue_id    = v;
                pls.is_best_level = (lvl == 0);

                auto pl_vertex = std::make_unique<PriceLevelVertex>(
                    pl_id, pl_id % w_count, pls);

                pl_vertex->edges().push_back({venue_vid, 1, 2, {}});
                data.vertices[venue_vid]->edges().push_back({pl_id, 1, 0, {}});
                data.total_edges += 2;

                for (uint64_t o = 0; o < params.orders_per_level; ++o) {
                    VertexId ord_id = next_id++;
                    order_ids.push_back(ord_id);

                    OrderVertex::State os;
                    os.limit_price  = price + static_cast<int64_t>(price_noise(rng));
                    os.quantity     = qty_dist(rng);
                    os.side         = side;
                    os.venue_id     = v;
                    os.trader_id    = rng() % 1000;
                    os.timestamp_ns = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());

                    auto ord_vertex = std::make_unique<OrderVertex>(
                        ord_id, ord_id % w_count, os);

                    ord_vertex->edges().push_back({pl_id, 1, 0, {}});
                    pl_vertex->edges().push_back({ord_id, 1, 1, {}});
                    data.total_edges += 2;

                    data.vertices.push_back(std::move(ord_vertex));
                    data.order_count++;
                }

                data.vertices.push_back(std::move(pl_vertex));
                data.price_level_count++;
            }
        }
    }

    for (uint32_t mm = 0; mm < params.market_maker_count; ++mm) {
        VertexId mm_id = next_id++;

        MarketMakerVertex::State mms;
        mms.mm_id          = mm;
        mms.net_position   = 0;
        mms.inventory_limit= 1'000'000;

        auto mm_vertex = std::make_unique<MarketMakerVertex>(
            mm_id, mm_id % w_count, mms);

        for (VertexId vid : venue_ids) {
            mm_vertex->edges().push_back({vid, 1, 0, {}});
            data.vertices[vid]->edges().push_back({mm_id, 1, 3, {}});
            data.total_edges += 2;
        }

        size_t sample = std::min((size_t)20, price_level_ids.size());
        for (size_t i = 0; i < sample; ++i) {
            VertexId pl_id = price_level_ids[rng() % price_level_ids.size()];
            mm_vertex->edges().push_back({pl_id, 1, 2, {}});
            data.total_edges++;
        }

        data.vertices.push_back(std::move(mm_vertex));
        data.mm_count++;
    }

    return data;
}

} 
