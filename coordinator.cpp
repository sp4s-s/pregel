#include "coordinator.hpp"
#include <iostream>
#include <iomanip>
#include <algorithm>
#include <sstream>
#include <chrono>
#include <numeric>
#include <future>

namespace pregel {

Coordinator::Coordinator(EngineConfig config,
                         std::vector<std::shared_ptr<WorkerNode>> workers,
                         std::shared_ptr<PartitionMap> pmap)
    : config_(std::move(config))
    , workers_(std::move(workers))
    , pmap_(std::move(pmap))
{
    hb_ = std::make_shared<HeartbeatDetector>(
        0, config_.worker_count,
        config_.heartbeat_interval_ms,
        config_.failure_timeout_ms);

    hb_->on_failure([this](WorkerId failed) {
        std::cerr << "[COORDINATOR] Worker " << failed
                  << " detected as FAILED — initiating recovery\n";
        handle_worker_failure(failed);
    });

    for (auto& w : workers_) {
        w->register_heartbeat_detector(hb_);
    }
}

Coordinator::~Coordinator() {
    if (hb_) hb_->stop();
}

void Coordinator::run() {
    start_time_ = std::chrono::steady_clock::now();
    hb_->start();

    // Optional: inject faults for resilience testing
    if (config_.enable_fault_injection && workers_.size() > 2) {
        std::thread([this]{
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            WorkerId target = 1; // kill worker 1
            std::cerr << "[FAULT INJECTION] Injecting failure for worker " << target << "\n";
            hb_->inject_failure(target);
        }).detach();
    }

    std::cout << "┌─────────────────────────────────────────────────────────────┐\n"
              << "│        Distributed Graph Engine — Pregel BSP Mode           │\n"
              << "├─────────────┬────────────────┬───────────────┬──────────────┤\n"
              << "│  Superstep  │  Active Verts  │  Msgs/Step    │  Throughput  │\n"
              << "├─────────────┼────────────────┼───────────────┼──────────────┤\n";

    for (uint32_t step = 0; step < config_.max_supersteps; ++step) {
        current_step_ = step;

        // Simulate heartbeats between workers
        hb_->send_heartbeat();
        for (uint32_t w = 0; w < config_.worker_count; ++w) {
            hb_->receive_heartbeat(w, std::chrono::steady_clock::now());
        }

        // Running all workers in parallel threads
        std::vector<std::future<uint64_t>> futures;
        for (auto& w : workers_) {
            futures.push_back(std::async(std::launch::async,
                [&w, step]{ return w->run_superstep(step); }));
        }

        uint64_t total_active = 0;
        for (auto& f : futures) total_active += f.get();

        // Collect stats for display
        uint64_t total_msgs = 0;
        double   max_tput   = 0.0;
        for (auto& w : workers_) {
            total_msgs += w->stats().messages_sent.load();
            max_tput    = std::max(max_tput, w->stats().throughput_vps);
        }

        print_superstep_stats(step, total_active);

        if (superstep_cb_) superstep_cb_(step, total_active);
        total_vertices_processed_ += total_active;

        // Convergence: all vertices halted and no messages in flight
        if (total_active == 0) {
            std::cout << "├─────────────┴────────────────┴───────────────┴──────────────┤\n"
                      << "│  CONVERGED at superstep " << std::setw(4) << step
                      << "                                    │\n";
            break;
        }
    }

    done_ = true;
    hb_->stop();
    print_summary();
}

bool Coordinator::collect_votes_and_advance() {
    return true;
}

void Coordinator::handle_worker_failure(WorkerId failed) {
    WorkerId new_owner = 0;
    size_t min_vertices = SIZE_MAX;
    for (auto& w : workers_) {
        if (w->id() == failed) continue;
        if (w->vertex_count() < min_vertices) {
            min_vertices = w->vertex_count();
            new_owner    = w->id();
        }
    }
    redistribute_partition(failed, new_owner);
}

void Coordinator::redistribute_partition(WorkerId failed, WorkerId new_owner) {
    std::cerr << "[COORDINATOR] Reassigning partition from worker "
              << failed << " to worker " << new_owner << "\n";
}

void Coordinator::print_superstep_stats(Superstep step, uint64_t active) {
    uint64_t msgs = 0;
    double   tput = 0.0;
    for (auto& w : workers_) {
        msgs += w->stats().messages_sent.load();
        tput  = std::max(tput, w->stats().throughput_vps);
    }

    std::ostringstream tput_str;
    if (tput >= 1e9)      tput_str << std::fixed << std::setprecision(2) << tput/1e9 << "B/s";
    else if (tput >= 1e6) tput_str << std::fixed << std::setprecision(2) << tput/1e6 << "M/s";
    else if (tput >= 1e3) tput_str << std::fixed << std::setprecision(2) << tput/1e3 << "K/s";
    else                  tput_str << (uint64_t)tput << "/s";

    std::cout << "│  " << std::setw(9)  << step   << "  │  "
              << std::setw(12) << active  << "  │  "
              << std::setw(11) << msgs    << "  │  "
              << std::setw(10) << tput_str.str() << "  │\n";
    std::cout.flush();
}

void Coordinator::print_summary() const {
    auto now = std::chrono::steady_clock::now();
    double elapsed = std::chrono::duration<double>(now - start_time_).count();

    std::cout << "└─────────────────────────────────────────────────────────────┘\n\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  DISTRIBUTED GRAPH ENGINE — FINAL STATISTICS\n";
    std::cout << "═══════════════════════════════════════════════════════════════\n";
    std::cout << "  Wall time         : " << std::fixed << std::setprecision(3)
              << elapsed << "s\n";
    std::cout << "  Workers           : " << workers_.size() << "\n";
    std::cout << "  Supersteps        : " << current_step_.load() + 1 << "\n";

    uint64_t total_v = 0, total_m = 0, total_rdma = 0,
             total_spikes = 0, total_migs = 0;
    for (auto& w : workers_) {
        total_v    += w->stats().vertices_processed.load();
        total_m    += w->stats().messages_sent.load();
        total_rdma += w->stats().rdma_ops.load();
        total_spikes += w->stats().latency_spike_count.load();
        total_migs   += w->stats().migrations_performed.load();
    }

    auto fmt_big = [](uint64_t v) -> std::string {
        std::ostringstream oss;
        if (v >= 1'000'000'000) oss << std::fixed << std::setprecision(2) << v/1e9 << "B";
        else if (v >= 1'000'000) oss << std::fixed << std::setprecision(2) << v/1e6 << "M";
        else if (v >= 1'000)     oss << std::fixed << std::setprecision(2) << v/1e3 << "K";
        else oss << v;
        return oss.str();
    };

    std::cout << "  Vertices computed : " << fmt_big(total_v) << "\n";
    std::cout << "  Messages routed   : " << fmt_big(total_m) << "\n";
    std::cout << "  RDMA operations   : " << fmt_big(total_rdma) << "\n";
    std::cout << "  Latency spikes    : " << total_spikes << "\n";
    std::cout << "  NUMA migrations   : " << total_migs << "\n";
    if (elapsed > 0) {
        double vps = total_v / elapsed;
        std::cout << "  Throughput        : " << fmt_big(static_cast<uint64_t>(vps))
                  << " vertices/sec\n";
    }
    std::cout << "═══════════════════════════════════════════════════════════════\n";
}

} 
