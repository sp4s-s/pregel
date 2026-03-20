#include "message_bus.hpp"
#include <algorithm>
#include <chrono>

namespace pregel {

std::atomic<uint64_t> MessageBus::barrier_counter_{0};
std::atomic<uint32_t> MessageBus::barrier_epoch_{0};

MessageBus::MessageBus(WorkerId local_id, uint32_t worker_count,
                       std::shared_ptr<PartitionMap> pmap, bool use_rdma)
    : local_id_(local_id)
    , worker_count_(worker_count)
    , use_rdma_(use_rdma)
    , pmap_(std::move(pmap))
    , outboxes_(worker_count)
{
    if (use_rdma_) {
        transport_ = std::make_unique<RdmaTransport>(local_id_, worker_count_, 256);
    }
}

MessageBus::~MessageBus() = default;

void MessageBus::enqueue(const Message& msg) {
    WorkerId dst_worker = pmap_->owner_of(msg.dst);
    std::lock_guard lk(outbox_mu_);
    outboxes_[dst_worker].push_back(msg);
}

void MessageBus::enqueue_batch(std::span<const Message> msgs) {
    std::lock_guard lk(outbox_mu_);
    for (const auto& m : msgs) {
        WorkerId dst_worker = pmap_->owner_of(m.dst);
        outboxes_[dst_worker].push_back(m);
    }
}

void MessageBus::flush(Superstep step) {
    std::vector<std::vector<Message>> local_outboxes;
    {
        std::lock_guard lk(outbox_mu_);
        local_outboxes.swap(outboxes_);
        outboxes_.resize(worker_count_);
    }

    for (uint32_t w = 0; w < worker_count_; ++w) {
        auto& box = local_outboxes[w];
        if (box.empty()) continue;

        msgs_sent_.fetch_add(box.size(), std::memory_order_relaxed);

        if (w == local_id_) {
            std::lock_guard lk(inbox_mu_);
            inbox_.insert(inbox_.end(), box.begin(), box.end());
        } else {
            if (use_rdma_ && transport_) {
                transport_->send_batch(w, box);
            }
        }
    }
}

std::vector<Message> MessageBus::drain_inbox() {
    if (use_rdma_ && transport_) {
        auto remote = transport_->receive_all();
        std::lock_guard lk(inbox_mu_);
        inbox_.insert(inbox_.end(), remote.begin(), remote.end());
    }

    std::lock_guard lk(inbox_mu_);
    msgs_received_.fetch_add(inbox_.size(), std::memory_order_relaxed);
    std::vector<Message> out;
    out.swap(inbox_);
    return out;
}

void MessageBus::barrier_sync(Superstep step, uint32_t total_workers) {
    uint32_t epoch = barrier_epoch_.load(std::memory_order_acquire);
    uint64_t arrived = barrier_counter_.fetch_add(1, std::memory_order_acq_rel) + 1;

    if (arrived == total_workers) {
        barrier_counter_.store(0, std::memory_order_release);
        barrier_epoch_.fetch_add(1, std::memory_order_release);
    } else {
        while (barrier_epoch_.load(std::memory_order_acquire) == epoch) {
            std::this_thread::yield();
        }
    }
}

} 
