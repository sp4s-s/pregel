#include "worker_node.hpp"
#include <algorithm>
#include <iostream>
#include <chrono>

namespace pregel {

WorkerNode::WorkerNode(WorkerId id,
                       EngineConfig config,
                       std::shared_ptr<PartitionMap>  pmap,
                       std::shared_ptr<MessageBus>    bus,
                       std::shared_ptr<NumaScheduler> scheduler)
    : id_(id)
    , config_(std::move(config))
    , pmap_(std::move(pmap))
    , bus_(std::move(bus))
    , scheduler_(std::move(scheduler))
{}

WorkerNode::~WorkerNode() { stop(); }

void WorkerNode::start() { running_ = true; }
void WorkerNode::stop()  { running_ = false; }

void WorkerNode::add_vertex(std::unique_ptr<Vertex> v) {
    std::lock_guard lk(vertices_mu_);
    VertexId vid = v->id();
    vertices_[vid] = std::move(v);
}

void WorkerNode::absorb_partition(std::vector<std::unique_ptr<Vertex>> migrated) {
    std::lock_guard lk(vertices_mu_);
    for (auto& v : migrated) {
        VertexId vid = v->id();
        v->set_owner(id_);
        pmap_->migrate(vid, id_);
        vertices_[vid] = std::move(v);
        stats_.migrations_performed.fetch_add(1, std::memory_order_relaxed);
    }
}

size_t WorkerNode::vertex_count() const {
    std::lock_guard lk(vertices_mu_);
    return vertices_.size();
}

void WorkerNode::register_heartbeat_detector(
    std::shared_ptr<HeartbeatDetector> hb)
{
    hb_detector_ = std::move(hb);
}

uint64_t WorkerNode::run_superstep(Superstep step) {
    auto t0 = std::chrono::steady_clock::now();

    auto inbox = bus_->drain_inbox();
    dispatch_messages(inbox);

    {
        std::lock_guard lk(vertices_mu_);
        std::vector<Task> tasks;
        tasks.reserve(vertices_.size());

        for (auto& [vid, vptr] : vertices_) {
            if (vptr->halted()) {
                std::lock_guard ilk(inbox_mu_);
                if (vertex_inbox_.find(vid) == vertex_inbox_.end()) continue;
                vptr->wake_up();
            }

            Vertex* raw = vptr.get();
            uint32_t numa_node = static_cast<uint32_t>(vid % config_.numa_nodes);

            tasks.push_back(Task{
                .task_id           = vid,
                .preferred_numa_node = numa_node,
                .fn = [this, raw, step, vid]() {
                    std::vector<Message> local_inbox;
                    {
                        std::lock_guard ilk(inbox_mu_);
                        auto it = vertex_inbox_.find(vid);
                        if (it != vertex_inbox_.end()) {
                            local_inbox = std::move(it->second);
                            vertex_inbox_.erase(it);
                        }
                    }
                    raw->compute(local_inbox, step);
                    stats_.vertices_processed.fetch_add(1, std::memory_order_relaxed);
                },
                .expected_duration = std::chrono::microseconds(50)
            });
        }
        scheduler_->submit_bulk(std::move(tasks));
    }

    scheduler_->wait_all();

    collect_and_route_outbox();

    bus_->flush(step);

    bus_->barrier_sync(step, config_.worker_count);

    stats_.supersteps_completed.fetch_add(1, std::memory_order_relaxed);

    uint64_t active = 0;
    {
        std::lock_guard lk(vertices_mu_);
        for (auto& [vid, vptr] : vertices_) {
            if (!vptr->halted()) active++;
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    double elapsed_s = std::chrono::duration<double>(t1 - t0).count();
    if (elapsed_s > 0.0) {
        uint64_t processed = stats_.vertices_processed.load();
        stats_.throughput_vps = processed / elapsed_s;
    }

    return active;
}

void WorkerNode::dispatch_messages(std::vector<Message>& msgs) {
    std::lock_guard lk(inbox_mu_);
    for (auto& msg : msgs) {
        vertex_inbox_[msg.dst].push_back(msg);
    }
    stats_.messages_received.fetch_add(msgs.size(), std::memory_order_relaxed);
}

void WorkerNode::collect_and_route_outbox() {
    std::lock_guard lk(vertices_mu_);
    for (auto& [vid, vptr] : vertices_) {
        auto& outbox = vptr->outbox();
        if (outbox.empty()) continue;

        stats_.messages_sent.fetch_add(outbox.size(), std::memory_order_relaxed);
        bus_->enqueue_batch(outbox);
        vptr->clear_outbox();
    }
}

} 
