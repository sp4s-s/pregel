#pragma once
#include "types.hpp"
#include "worker_node.hpp"
#include "heartbeat.hpp"
#include <vector>
#include <memory>
#include <atomic>
#include <mutex>
#include <functional>
#include <optional>

namespace pregel {

struct WorkerVote {
    WorkerId worker_id;
    uint64_t active_vertices;
    uint64_t messages_in_transit;
    bool     ready;
};


class Coordinator {
public:
    Coordinator(EngineConfig config,
                std::vector<std::shared_ptr<WorkerNode>> workers,
                std::shared_ptr<PartitionMap> pmap);

    ~Coordinator();

    void run();

    void on_superstep_complete(std::function<void(Superstep, uint64_t active)> cb) {
        superstep_cb_ = std::move(cb);
    }

    Superstep current_superstep() const { return current_step_.load(); }
    bool      is_done()           const { return done_.load(); }

    void print_summary() const;

private:
    bool collect_votes_and_advance();
    void handle_worker_failure(WorkerId failed);
    void redistribute_partition(WorkerId failed, WorkerId new_owner);
    void print_superstep_stats(Superstep step, uint64_t active);

    EngineConfig config_;
    std::vector<std::shared_ptr<WorkerNode>>    workers_;
    std::shared_ptr<PartitionMap>               pmap_;
    std::shared_ptr<HeartbeatDetector>          hb_;

    std::atomic<Superstep> current_step_{0};
    std::atomic<bool>      done_{false};

    //Stats
    uint64_t total_vertices_processed_{0};
    uint64_t total_messages_routed_{0};
    std::chrono::steady_clock::time_point start_time_;

    std::function<void(Superstep, uint64_t)> superstep_cb_;
    mutable std::mutex stats_mu_;
};

} 
