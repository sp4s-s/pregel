#pragma once
#include "types.hpp"
#include "vertex.hpp"
#include "message_bus.hpp"
#include "heartbeat.hpp"
#include "numa_scheduler.hpp"
#include <vector>
#include <unordered_map>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <functional>

namespace pregel {


class WorkerNode {
public:
    WorkerNode(WorkerId id,
               EngineConfig config,
               std::shared_ptr<PartitionMap>   pmap,
               std::shared_ptr<MessageBus>     bus,
               std::shared_ptr<NumaScheduler>  scheduler);
    ~WorkerNode();

    void start();
    void stop();

    void add_vertex(std::unique_ptr<Vertex> v);
    void absorb_partition(std::vector<std::unique_ptr<Vertex>> migrated);
    size_t vertex_count() const;

    uint64_t run_superstep(Superstep step);

    void register_heartbeat_detector(std::shared_ptr<HeartbeatDetector> hb);

    const WorkerStats& stats() const { return stats_; }
    WorkerId id() const { return id_; }

private:
    void dispatch_messages(std::vector<Message>& msgs);
    void collect_and_route_outbox();
    void check_latency_spikes();

    WorkerId    id_;
    EngineConfig config_;
    std::shared_ptr<PartitionMap>   pmap_;
    std::shared_ptr<MessageBus>     bus_;
    std::shared_ptr<NumaScheduler>  scheduler_;
    std::shared_ptr<HeartbeatDetector> hb_detector_;

    mutable std::mutex                              vertices_mu_;
    std::unordered_map<VertexId, std::unique_ptr<Vertex>> vertices_;

    WorkerStats stats_;
    std::atomic<bool> running_{false};

    std::unordered_map<VertexId, std::vector<Message>> vertex_inbox_;
    std::mutex inbox_mu_;
};

}
