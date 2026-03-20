#include "graph_engine.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <vector>

// i promise ill never format it better

int main() {
    std::cout << "╔══════════════════════════════════════════════════════════╗\n"
              << "║         Pregel Engine — Throughput Benchmark             ║\n"
              << "╠══════════════════╦═══════════════════╦═══════════════════╣\n"
              << "║  Workers         ║  Vertices/sec     ║  Scaling          ║\n"
              << "╠══════════════════╬═══════════════════╬═══════════════════╣\n";

    std::vector<uint32_t> worker_counts = {1, 2, 4};
    double baseline = 0.0;

    for (uint32_t wc : worker_counts) {
        pregel::EngineConfig cfg;
        cfg.worker_count          = wc;
        cfg.numa_nodes            = std::max(1u, wc / 2);
        cfg.max_supersteps        = 20;
        cfg.enable_rdma_sim       = true;
        cfg.enable_fault_injection= false;

        pregel::GraphEngine engine(cfg);
        double vps = engine.benchmark(10);

        if (wc == 1) baseline = vps;

        std::ostringstream vps_str, scale_str;
        if      (vps >= 1e9) vps_str << std::fixed << std::setprecision(2) << vps/1e9 << "B/s";
        else if (vps >= 1e6) vps_str << std::fixed << std::setprecision(2) << vps/1e6 << "M/s";
        else if (vps >= 1e3) vps_str << std::fixed << std::setprecision(2) << vps/1e3 << "K/s";
        else                 vps_str << (uint64_t)vps << "/s";

        double scaling = (baseline > 0) ? vps / baseline : 1.0;
        scale_str << std::fixed << std::setprecision(2) << scaling << "x";

        std::cout << "║  " << std::setw(16) << wc << "  ║  "
                  << std::setw(17) << vps_str.str() << "  ║  "
                  << std::setw(17) << scale_str.str() << "  ║\n";
    }

    std::cout << "╚══════════════════╩═══════════════════╩═══════════════════╝\n";
    return 0;
}
