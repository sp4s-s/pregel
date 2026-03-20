#pragma once
#include "types.hpp"
#include "rdma_transport.hpp"
#include <vector>
#include <shared_mutex>
#include <unordered_map>
#include <mutex>
#include <atomic>
#include <memory>
#include <functional>

namespace pregel {

class PartitionMap {
public:
    explicit PartitionMap(uint32_t worker_count)
        : worker_count_(worker_count) {}

    WorkerId owner_of(VertexId v) const {
        // First check migration overrides
        std::shared_lock lk(mu_);
        auto it = overrides_.find(v);
        if (it != overrides_.end()) return it->second;
        return static_cast<WorkerId>(v % worker_count_);
    }

    void migrate(VertexId v, WorkerId new_owner) {
        std::unique_lock lk(mu_);
        overrides_[v] = new_owner;
    }

    uint32_t worker_count() const { return worker_count_; }

private:
    uint32_t worker_count_;
    mutable std::shared_mutex                   mu_;
    std::unordered_map<VertexId, WorkerId>      overrides_;
};

class MessageBus {
public:
    MessageBus(WorkerId local_id,
               uint32_t worker_count,
               std::shared_ptr<PartitionMap> pmap,
               bool use_rdma);
    ~MessageBus();

    void enqueue(const Message& msg);
    void enqueue_batch(std::span<const Message> msgs);

    void flush(Superstep step);

    std::vector<Message> drain_inbox();

    void barrier_sync(Superstep step, uint32_t total_workers);

    uint64_t messages_sent()    const { return msgs_sent_.load(); }
    uint64_t messages_received()const { return msgs_received_.load(); }

private:
    WorkerId   local_id_;
    uint32_t   worker_count_;
    bool       use_rdma_;

    std::shared_ptr<PartitionMap>   pmap_;
    std::unique_ptr<RdmaTransport>  transport_;

    std::vector<std::vector<Message>> outboxes_;
    std::mutex                         outbox_mu_;

    std::vector<Message>    inbox_;
    std::mutex              inbox_mu_;

    std::atomic<uint64_t>   msgs_sent_{0};
    std::atomic<uint64_t>   msgs_received_{0};

    static std::atomic<uint64_t> barrier_counter_;
    static std::atomic<uint32_t> barrier_epoch_;
};

} 
