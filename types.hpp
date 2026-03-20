#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <memory>
#include <functional>
#include <atomic>
#include <chrono>
#include <array>
#include <span>

namespace pregel {

using VertexId  = uint64_t;
using WorkerId  = uint32_t;
using Superstep = uint64_t;
using Timestamp = std::chrono::steady_clock::time_point;

enum class Side : uint8_t { BID = 0, ASK = 1 };

struct PriceLevel {
    int64_t  price; // fixed-point: actual_price * 1e6
    uint64_t quantity;
    uint32_t order_count;
    uint8_t  pad[4];
};

enum class VertexType : uint8_t {
    ORDER       = 0,
    PRICE_LEVEL = 1,
    VENUE       = 2,
    MARKET_MAKER= 3
};

enum class MsgType : uint8_t {
    ORDER_FILL       = 0,   
    PRICE_UPDATE     = 1,   
    CANCEL_ACK       = 2,   
    POSITION_UPDATE  = 3,   
    ARBITRAGE_SIGNAL = 4,
    HEARTBEAT        = 5,
    BARRIER_SYNC     = 6,
};

struct alignas(64) Message {
    VertexId  src;
    VertexId  dst;
    MsgType   type;
    uint8_t   pad[3];
    Superstep step;
    union Payload {
        struct Fill {
            int64_t  price;
            uint64_t qty;
            uint64_t trade_id;
        } fill;
        struct PriceUpd {
            int64_t  bid;
            int64_t  ask;
            uint64_t bid_qty;
            uint64_t ask_qty;
        } price;
        struct ArbSig {
            int64_t  spread;      
            VertexId venue_a;
            VertexId venue_b;
        } arb;
        uint8_t raw[24];
    } payload;
};
static_assert(sizeof(Message) <= 64, "Message must fit in a cache line");

struct NumaNode {
    uint32_t node_id;
    uint32_t core_count;
    uint64_t memory_bytes;
    std::vector<uint32_t> cpu_ids;
};

struct EngineConfig {
    uint32_t worker_count          = 4;
    uint32_t numa_nodes            = 2;
    uint64_t vertex_count          = 1'000'000;
    uint64_t edge_count_per_vertex = 8;
    uint32_t max_supersteps        = 100;
    uint32_t heartbeat_interval_ms = 500;
    uint32_t failure_timeout_ms    = 2000;
    uint32_t rdma_buffer_size_kb   = 256;
    bool     enable_rdma_sim       = true;
    bool     enable_fault_injection= false;
    double   fault_probability     = 0.001;
    std::string graph_type         = "order_book"; 
};

struct WorkerStats {
    std::atomic<uint64_t> vertices_processed{0};
    std::atomic<uint64_t> messages_sent{0};
    std::atomic<uint64_t> messages_received{0};
    std::atomic<uint64_t> bytes_transferred{0};
    std::atomic<uint64_t> supersteps_completed{0};
    std::atomic<uint64_t> latency_spike_count{0};
    std::atomic<uint64_t> migrations_performed{0};
    std::atomic<uint64_t> rdma_ops{0};
    double                throughput_vps{0.0};   
};

} 
