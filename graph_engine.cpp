#include "graph_engine.hpp"
#include <iostream>
#include <algorithm>
#include <future>

namespace pregel {

GraphEngine::GraphEngine(EngineConfig config)
    : config_(std::move(config))
{}

GraphEngine::~GraphEngine() = default;

void GraphEngine::run() {
    setup_infrastructure();
    load_graph();
    coordinator_->run();
}

double GraphEngine::benchmark(uint32_t supersteps) {
    setup_infrastructure();
    load_graph();

    auto t0 = std::chrono::steady_clock::now();
    uint64_t total_verts = 0;

    for (uint32_t step = 0; step < supersteps; ++step) {
        std::vector<std::future<uint64_t>> futures;
        for (auto& w : workers_) {
            futures.push_back(std::async(std::launch::async,
                [&w, step]{ return w->run_superstep(step); }));
        }
        for (auto& f : futures) total_verts += f.get();
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(t1 - t0).count();
    return (elapsed > 0) ? total_verts / elapsed : 0.0;
}

void GraphEngine::setup_infrastructure() {
    pmap_ = std::make_shared<PartitionMap>(config_.worker_count);

    uint32_t threads_per_node = std::max(1u,
        std::thread::hardware_concurrency() / config_.numa_nodes);
    scheduler_ = std::make_shared<NumaScheduler>(config_.numa_nodes, threads_per_node);
    scheduler_->start();

    buses_.resize(config_.worker_count);
    workers_.resize(config_.worker_count);

    for (uint32_t i = 0; i < config_.worker_count; ++i) {
        buses_[i] = std::make_shared<MessageBus>(
            i, config_.worker_count, pmap_, config_.enable_rdma_sim);
        workers_[i] = std::make_shared<WorkerNode>(
            i, config_, pmap_, buses_[i], scheduler_);
        workers_[i]->start();
    }

    coordinator_ = std::make_unique<Coordinator>(config_, workers_, pmap_);
}

void GraphEngine::load_graph() {
    std::cout << "[Engine] Building order-book graph...\n";

    OrderBookGraphParams params;
    params.venue_count       = 5;
    params.price_levels      = 10;
    params.orders_per_level  = 20;
    params.market_maker_count= 8;
    params.worker_count      = config_.worker_count;
    params.seed              = 0xC0FFEE42;

    auto data = build_order_book_graph(params);

    std::cout << "[Engine] Graph built:\n"
              << "  Venues        : " << data.venue_count       << "\n"
              << "  Price levels  : " << data.price_level_count  << "\n"
              << "  Orders        : " << data.order_count         << "\n"
              << "  Market makers : " << data.mm_count            << "\n"
              << "  Total vertices: " << data.vertices.size()     << "\n"
              << "  Total edges   : " << data.total_edges         << "\n\n";

    distribute_vertices(data);
}

void GraphEngine::distribute_vertices(GraphData& data) {
    for (auto& vptr : data.vertices) {
        WorkerId owner = pmap_->owner_of(vptr->id());
        workers_[owner]->add_vertex(std::move(vptr));
    }
    data.vertices.clear();

    for (uint32_t i = 0; i < config_.worker_count; ++i) {
        std::cout << "[Engine] Worker " << i << " owns "
                  << workers_[i]->vertex_count() << " vertices\n";
    }
    std::cout << "\n";
}

} 
