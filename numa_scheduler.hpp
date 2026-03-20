#pragma once
#include "types.hpp"
#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <queue>
#include <atomic>
#include <condition_variable>
#include <optional>
#include <chrono>

namespace pregel {


struct Task {
    uint64_t                    task_id;
    uint32_t                    preferred_numa_node;
    std::function<void()>       fn;
    std::chrono::microseconds   expected_duration{100};
};

struct LatencyRecord {
    uint64_t              task_id;
    uint32_t              numa_node;
    std::chrono::microseconds actual;
    std::chrono::microseconds expected;
    bool                  is_spike;
};

class NumaScheduler {
public:
    NumaScheduler(uint32_t numa_nodes, uint32_t threads_per_node);
    ~NumaScheduler();

    void start();
    void stop();

    void submit(Task t);

    void submit_bulk(std::vector<Task> tasks);

    void wait_all();

    void migrate_vertex(VertexId v, uint32_t& current_numa_node);

    std::vector<LatencyRecord> spike_history() const;
    uint64_t spike_count()   const { return spike_count_.load(); }
    uint64_t migrate_count() const { return migrate_count_.load(); }

private:
    struct NumaQueue {
        std::queue<Task>        tasks;
        mutable std::mutex      mu;
        std::condition_variable cv;
        std::atomic<uint64_t>   active_tasks{0};
        std::vector<std::thread>threads;
    };

    void worker_thread(uint32_t numa_node, uint32_t thread_idx);
    uint32_t least_loaded_node() const;

    uint32_t numa_nodes_;
    uint32_t threads_per_node_;
    std::vector<std::unique_ptr<NumaQueue>> queues_;

    std::atomic<bool>     running_{false};
    std::atomic<uint64_t> total_submitted_{0};
    std::atomic<uint64_t> total_completed_{0};
    std::atomic<uint64_t> spike_count_{0};
    std::atomic<uint64_t> migrate_count_{0};

    mutable std::mutex             spike_mu_;
    std::vector<LatencyRecord>     spikes_;

    static constexpr double SPIKE_THRESHOLD = 3.0;  // 3× expected = spike
};

} 
