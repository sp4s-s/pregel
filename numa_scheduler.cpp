#include "numa_scheduler.hpp"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace pregel {

NumaScheduler::NumaScheduler(uint32_t numa_nodes, uint32_t threads_per_node)
    : numa_nodes_(numa_nodes), threads_per_node_(threads_per_node)
{
    for (uint32_t i = 0; i < numa_nodes_; ++i) {
        queues_.push_back(std::make_unique<NumaQueue>());
    }
}

NumaScheduler::~NumaScheduler() {
    stop();
}

void NumaScheduler::start() {
    running_ = true;
    for (uint32_t node = 0; node < numa_nodes_; ++node) {
        auto& q = queues_[node];
        for (uint32_t t = 0; t < threads_per_node_; ++t) {
            q->threads.emplace_back([this, node, t]{
                worker_thread(node, t);
            });
        }
    }
}

void NumaScheduler::stop() {
    running_ = false;
    for (auto& q : queues_) {
        q->cv.notify_all();
        for (auto& t : q->threads) {
            if (t.joinable()) t.join();
        }
        q->threads.clear();
    }
}

void NumaScheduler::submit(Task t) {
    uint32_t node = t.preferred_numa_node % numa_nodes_;
    auto& q = *queues_[node];
    {
        std::lock_guard lk(q.mu);
        q.tasks.push(std::move(t));
        q.active_tasks.fetch_add(1, std::memory_order_relaxed);
    }
    total_submitted_.fetch_add(1, std::memory_order_relaxed);
    q.cv.notify_one();
}

void NumaScheduler::submit_bulk(std::vector<Task> tasks) {
    for (auto& t : tasks) submit(std::move(t));
}

void NumaScheduler::wait_all() {
    // Spin until all submitted == all completed
    while (total_completed_.load(std::memory_order_acquire) <
           total_submitted_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void NumaScheduler::migrate_vertex(VertexId /*v*/, uint32_t& current_numa_node) {
    uint32_t new_node = least_loaded_node();
    if (new_node != current_numa_node) {
        current_numa_node = new_node;
        migrate_count_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::vector<LatencyRecord> NumaScheduler::spike_history() const {
    std::lock_guard lk(spike_mu_);
    return spikes_;
}

uint32_t NumaScheduler::least_loaded_node() const {
    uint32_t best = 0;
    uint64_t min_load = queues_[0]->active_tasks.load(std::memory_order_relaxed);
    for (uint32_t i = 1; i < numa_nodes_; ++i) {
        uint64_t load = queues_[i]->active_tasks.load(std::memory_order_relaxed);
        if (load < min_load) {
            min_load = load;
            best = i;
        }
    }
    return best;
}

void NumaScheduler::worker_thread(uint32_t numa_node, uint32_t /*thread_idx*/) {
    auto& q = *queues_[numa_node];

    while (running_) {
        Task task;
        {
            std::unique_lock lk(q.mu);
            q.cv.wait_for(lk, std::chrono::milliseconds(5),
                          [&]{ return !q.tasks.empty() || !running_; });
            if (q.tasks.empty()) continue;
            task = std::move(q.tasks.front());
            q.tasks.pop();
        }

        auto t0 = std::chrono::steady_clock::now();
        task.fn();
        auto t1 = std::chrono::steady_clock::now();

        auto actual = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0);
        bool is_spike = (actual.count() >
                         task.expected_duration.count() * SPIKE_THRESHOLD);

        if (is_spike) {
            spike_count_.fetch_add(1, std::memory_order_relaxed);
            std::lock_guard lk(spike_mu_);
            spikes_.push_back({task.task_id, numa_node,
                               actual, task.expected_duration, true});
            // Cap history
            if (spikes_.size() > 1000) spikes_.erase(spikes_.begin());
        }

        q.active_tasks.fetch_sub(1, std::memory_order_relaxed);
        total_completed_.fetch_add(1, std::memory_order_release);
    }
}

} 
