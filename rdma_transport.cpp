#include "rdma_transport.hpp"
#include <cstring>
#include <algorithm>
#include <stdexcept>
#include <chrono>

namespace pregel {

QueuePair::QueuePair(WorkerId local, WorkerId remote, size_t buf_size_bytes)
    : local_(local), remote_(remote)
    , send_buf_(buf_size_bytes)
    , recv_buf_(buf_size_bytes)
{
    net_thread_ = std::thread([this]{ network_loop(); });
}

QueuePair::~QueuePair() {
    stop_ = true;
    cv_.notify_all();
    if (net_thread_.joinable()) net_thread_.join();
}

bool QueuePair::post_write(const void* src, size_t len, uint64_t wr_id) {
    if (len > sizeof(Message)) return false;
    Message msg{};
    std::memcpy(&msg, src, len);

    auto deliver_at = std::chrono::steady_clock::now()
                    + std::chrono::microseconds(RDMA_LATENCY_US);

    {
        std::lock_guard lk(flight_mu_);
        in_flight_.push({wr_id, msg, deliver_at});
    }
    pending_wr_.fetch_add(1, std::memory_order_relaxed);
    cv_.notify_one();
    return true;
}

void QueuePair::network_loop() {
    while (!stop_) {
        std::unique_lock lk(flight_mu_);
        cv_.wait_for(lk, std::chrono::milliseconds(1),
                     [this]{ return stop_ || !in_flight_.empty(); });

        auto now = std::chrono::steady_clock::now();
        while (!in_flight_.empty() && in_flight_.front().deliver_at <= now) {
            auto pw = in_flight_.front();
            in_flight_.pop();
            lk.unlock();

            {
                std::lock_guard ilk(mu_);
                inbox_.push(pw.msg);
            }

            {
                std::lock_guard clk(cq_mu_);
                cq_.push_back({pw.wr_id, sizeof(Message), true});
            }
            pending_wr_.fetch_sub(1, std::memory_order_relaxed);

            lk.lock();
            now = std::chrono::steady_clock::now();
        }
    }
}

std::vector<RdmaCompletion> QueuePair::poll_cq(int max_completions) {
    std::lock_guard lk(cq_mu_);
    int n = std::min((int)cq_.size(), max_completions);
    std::vector<RdmaCompletion> result(cq_.begin(), cq_.begin() + n);
    cq_.erase(cq_.begin(), cq_.begin() + n);
    return result;
}

void QueuePair::flush() {
    while (pending_wr_.load(std::memory_order_relaxed) > 0) {
        std::this_thread::sleep_for(std::chrono::microseconds(10));
    }
}

std::vector<Message> QueuePair::drain_received() {
    std::lock_guard lk(mu_);
    std::vector<Message> result;
    while (!inbox_.empty()) {
        result.push_back(inbox_.front());
        inbox_.pop();
    }
    return result;
}

RdmaTransport::RdmaTransport(WorkerId local_id, uint32_t worker_count,
                             size_t buf_size_kb)
    : local_id_(local_id), worker_count_(worker_count)
{
    qps_.resize(worker_count);
    for (uint32_t i = 0; i < worker_count; ++i) {
        if (i == local_id) continue;
        qps_[i] = std::make_unique<QueuePair>(local_id, i, buf_size_kb * 1024);
    }
}

bool RdmaTransport::send(WorkerId dst, const Message& msg) {
    if (dst >= worker_count_ || dst == local_id_ || !qps_[dst]) return false;
    uint64_t wr_id = rdma_ops_.fetch_add(1, std::memory_order_relaxed);
    bool ok = qps_[dst]->post_write(&msg, sizeof(msg), wr_id);
    if (ok) bytes_sent_.fetch_add(sizeof(msg), std::memory_order_relaxed);
    return ok;
}

bool RdmaTransport::send_batch(WorkerId dst, std::span<const Message> msgs) {
    if (dst >= worker_count_ || dst == local_id_ || !qps_[dst]) return false;
    bool ok = true;
    for (const auto& m : msgs) ok &= send(dst, m);
    return ok;
}

std::vector<Message> RdmaTransport::receive_from(WorkerId src) {
    if (src >= worker_count_ || src == local_id_ || !qps_[src])
        return {};
    return qps_[src]->drain_received();
}

std::vector<Message> RdmaTransport::receive_all() {
    std::vector<Message> all;
    for (uint32_t i = 0; i < worker_count_; ++i) {
        if (i == local_id_ || !qps_[i]) continue;
        auto msgs = qps_[i]->drain_received();
        all.insert(all.end(), msgs.begin(), msgs.end());
    }
    return all;
}

uint64_t RdmaTransport::poll_completions() {
    uint64_t total = 0;
    for (auto& qp : qps_) {
        if (!qp) continue;
        total += qp->poll_cq().size();
    }
    return total;
}

}
