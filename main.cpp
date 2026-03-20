#include "graph_engine.hpp"
#include <iostream>
#include <string>
#include <cstdlib>

static void print_banner() {
    std::cout << R"(
╔═══════════════════════════════════════════════════════════════════╗
║   Distributed Graph Processing Engine  —  Pregel / Jane Street   ║
║   C++20 · RDMA-Simulated · NUMA-Aware · Phi Accrual FD           ║
╚═══════════════════════════════════════════════════════════════════╝
)" << "\n";
}

int main(int argc, char* argv[]) {
    print_banner();

    pregel::EngineConfig cfg;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--workers"    && i+1 < argc) cfg.worker_count    = std::atoi(argv[++i]);
        if (arg == "--steps"      && i+1 < argc) cfg.max_supersteps  = std::atoi(argv[++i]);
        if (arg == "--numa-nodes" && i+1 < argc) cfg.numa_nodes      = std::atoi(argv[++i]);
        if (arg == "--fault"                   ) cfg.enable_fault_injection = true;
        if (arg == "--no-rdma"                 ) cfg.enable_rdma_sim = false;
        if (arg == "--help") {
            std::cout << "Usage: pregel_engine [options]\n"
                      << "  --workers N     Number of simulated workers (default: 4)\n"
                      << "  --steps N       Max supersteps (default: 100)\n"
                      << "  --numa-nodes N  NUMA topology (default: 2)\n"
                      << "  --fault         Enable fault injection (kill worker 1 at t=500ms)\n"
                      << "  --no-rdma       Disable RDMA simulation\n";
            return 0;
        }
    }

    std::cout << "Configuration:\n"
              << "  Workers          : " << cfg.worker_count << "\n"
              << "  NUMA nodes       : " << cfg.numa_nodes << "\n"
              << "  Max supersteps   : " << cfg.max_supersteps << "\n"
              << "  RDMA simulation  : " << (cfg.enable_rdma_sim ? "ON" : "OFF") << "\n"
              << "  Fault injection  : " << (cfg.enable_fault_injection ? "ON" : "OFF") << "\n\n";

    pregel::GraphEngine engine(cfg);
    engine.run();

    return 0;
}
