#include "heartbeat.hpp"
#include <cmath>
#include <algorithm>
#include <iostream>
#include <numeric>

namespace pregel {

HeartbeatDetector::HeartbeatDetector(WorkerId local_id, uint32_t worker_count,
                                     uint32_t interval_ms, uint32_t timeout_ms,
                                     double phi_threshold)
    : local_id_(local_id)
    , worker_count_(worker_count)
    , interval_ms_(interval_ms)
    , timeout_ms_(timeout_ms)
    , phi_threshold_(phi_threshold)
{
    std::lock_guard lk(mu_);
    for (uint32_t i = 0; i < worker_count_; ++i) {
        if (i == local_id_) continue;
        HeartbeatRecord rec;
        rec.worker_id = i;
        rec.last_seen = std::chrono::steady_clock::now();
        rec.state     = NodeState::ALIVE;
        // Seed intervals with expected value so phi starts near 0
        rec.intervals.assign(8, static_cast<double>(interval_ms_));
        records_[i] = rec;
    }
}

HeartbeatDetector::~HeartbeatDetector() {
    stop();
}

void HeartbeatDetector::start() {
    running_ = true;
    monitor_thread_ = std::thread([this]{ monitor_loop(); });
}

void HeartbeatDetector::stop() {
    running_ = false;
    if (monitor_thread_.joinable()) monitor_thread_.join();
}

void HeartbeatDetector::send_heartbeat() {
    // Note: practically this sends a UDP/RDMA packet to all peers.
    // Current impl  it's a no-op — peers call receive_heartbeat() directly.
}

void HeartbeatDetector::receive_heartbeat(WorkerId from, Timestamp sent_at) {
    auto now = std::chrono::steady_clock::now();
    std::lock_guard lk(mu_);
    auto it = records_.find(from);
    if (it == records_.end()) return;

    auto& rec = it->second;
    if (rec.beat_count > 0) {
        double interval = std::chrono::duration<double, std::milli>(
                              now - rec.last_seen).count();
        rec.intervals.push_back(interval);
        if (rec.intervals.size() > HeartbeatRecord::WINDOW)
            rec.intervals.erase(rec.intervals.begin());
    }
    rec.last_seen = now;
    rec.beat_count++;

    NodeState prev = rec.state;
    rec.state = NodeState::ALIVE;
    rec.phi   = 0.0;

    if (prev == NodeState::FAILED || prev == NodeState::SUSPECTED) {
        if (recovery_cb_) recovery_cb_(from);
    }
}

void HeartbeatDetector::inject_failure(WorkerId w) {
    std::lock_guard lk(mu_);
    auto it = records_.find(w);
    if (it == records_.end()) return;
    // Simulate: last_seen far in the past
    it->second.last_seen -= std::chrono::milliseconds(timeout_ms_ * 10);
}

NodeState HeartbeatDetector::state_of(WorkerId w) const {
    std::lock_guard lk(mu_);
    auto it = records_.find(w);
    if (it == records_.end()) return NodeState::ALIVE;
    return it->second.state;
}

double HeartbeatDetector::phi_of(WorkerId w) const {
    std::lock_guard lk(mu_);
    auto it = records_.find(w);
    if (it == records_.end()) return 0.0;
    return it->second.phi;
}

std::vector<WorkerId> HeartbeatDetector::failed_workers() const {
    std::lock_guard lk(mu_);
    std::vector<WorkerId> failed;
    for (auto& [wid, rec] : records_) {
        if (rec.state == NodeState::FAILED) failed.push_back(wid);
    }
    return failed;
}

double HeartbeatDetector::compute_phi(const HeartbeatRecord& rec) const {
    if (rec.intervals.empty()) return 0.0;

    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double, std::milli>(
                         now - rec.last_seen).count();

                        //  
    // Gaussian approximation: phi = -log10(1 - CDF(elapsed))
    double mean = std::accumulate(rec.intervals.begin(), rec.intervals.end(), 0.0)
                / rec.intervals.size();

    double var = 0.0;
    for (double v : rec.intervals) var += (v - mean) * (v - mean);
    var /= rec.intervals.size();
    double sigma = std::sqrt(var + 1e-9); // avoid div-by-zero

    double y = (elapsed - mean) / sigma;
    // CDF approximation: 0.5 * erfc(-y / sqrt(2))
    double cdf = 0.5 * std::erfc(-y / std::sqrt(2.0));
    double phi = -std::log10(std::max(1.0 - cdf, 1e-15));
    return phi;
}

void HeartbeatDetector::monitor_loop() {
    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_ / 4));

        std::lock_guard lk(mu_);
        for (auto& [wid, rec] : records_) {
            check_worker(rec);
        }
    }
}

void HeartbeatDetector::check_worker(HeartbeatRecord& rec) {
    if (rec.state == NodeState::FAILED) return;

    rec.phi = compute_phi(rec);

    auto now = std::chrono::steady_clock::now();
    double ms_since = std::chrono::duration<double, std::milli>(
                          now - rec.last_seen).count();

    NodeState prev = rec.state;

    if (rec.phi >= phi_threshold_ || ms_since > timeout_ms_) {
        rec.state = NodeState::FAILED;
        if (prev != NodeState::FAILED && failure_cb_) {
            failure_cb_(rec.worker_id);
        }
    } else if (rec.phi >= phi_threshold_ * 0.5) {
        rec.state = NodeState::SUSPECTED;
    } else {
        rec.state = NodeState::ALIVE;
    }
}

}
