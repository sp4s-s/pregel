#pragma once
#include "types.hpp"
#include <atomic>
#include <thread>
#include <mutex>
#include <unordered_map>
#include <functional>
#include <chrono>
#include <vector>

namespace pregel {

enum class NodeState : uint8_t {
    ALIVE      = 0,
    SUSPECTED  = 1,
    FAILED     = 2,
    RECOVERED  = 3,
};

struct HeartbeatRecord {
    WorkerId  worker_id;
    Timestamp last_seen;
    NodeState state{NodeState::ALIVE};
    double    phi{0.0};                // suspicion level
    uint64_t  beat_count{0};

    // Rolling window of inter-arrival times (ms) for phi estimation
    std::vector<double>  intervals;
    static constexpr size_t WINDOW = 16;
};

using FailureCallback  = std::function<void(WorkerId failed)>;
using RecoveryCallback = std::function<void(WorkerId recovered)>;

class HeartbeatDetector {
public:
    HeartbeatDetector(WorkerId local_id,
                      uint32_t worker_count,
                      uint32_t interval_ms   = 500,
                      uint32_t timeout_ms    = 2000,
                      double   phi_threshold = 8.0);

    ~HeartbeatDetector();

    void send_heartbeat();

    void receive_heartbeat(WorkerId from, Timestamp sent_at);

    void on_failure (FailureCallback  cb) { failure_cb_  = std::move(cb); }
    void on_recovery(RecoveryCallback cb) { recovery_cb_ = std::move(cb); }

    void inject_failure(WorkerId w);

    NodeState state_of(WorkerId w) const;
    double    phi_of  (WorkerId w) const;
    bool      is_alive(WorkerId w) const { return state_of(w) == NodeState::ALIVE; }

    std::vector<WorkerId> failed_workers() const;

    void start();
    void stop();

private:
    void monitor_loop();
    void check_worker(HeartbeatRecord& rec);
    double compute_phi(const HeartbeatRecord& rec) const;

    WorkerId  local_id_;
    uint32_t  worker_count_;
    uint32_t  interval_ms_;
    uint32_t  timeout_ms_;
    double    phi_threshold_;

    mutable std::mutex                            mu_;
    std::unordered_map<WorkerId, HeartbeatRecord> records_;
    FailureCallback                               failure_cb_;
    RecoveryCallback                              recovery_cb_;

    std::thread       monitor_thread_;
    std::atomic<bool> running_{false};
};

}

// Failure detector 
// Current setup mimics
// A Phi Accrual-style failure detector.
// Each worker sends periodic heartbeats; the detector tracks arrival times
// and computes a suspicion level phi. When phi exceeds a threshold the
// worker is marked 'FAILED' and a recovery callback fires.

//Extension: not doing it 
        // in real cluster this sits on a dedicated management thread per node
        // and communicates via a side-channel (not the data path(hot-path)).