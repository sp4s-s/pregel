set -e

BOLD=$(tput bold 2>/dev/null || echo "")
RESET=$(tput sgr0 2>/dev/null || echo "")
GREEN=$(tput setaf 2 2>/dev/null || echo "")
CYAN=$(tput setaf 6 2>/dev/null || echo "")

log() { echo "${BOLD}${CYAN}[pregel]${RESET} $*"; }
ok()  { echo "${BOLD}${GREEN}[ok]${RESET} $*"; }

MODE="${1:-main}"

if [ "$MODE" = "help" ]; then
    echo "Usage: bash run.sh [mode]"
    echo ""
    echo "Modes:"
    echo "  main   (default) — Run full Pregel engine on order-book graph"
    echo "  bench            — Throughput scaling benchmark (1→2→4 workers)"
    echo "  fault            — Run with fault injection (kills worker 1 mid-run)"
    echo "  help             — Show this message"
    exit 0
fi

if ! command -v g++ &>/dev/null; then
    echo "g++ not found. Install with: sudo apt-get install g++"
    exit 1
fi

CXX_VERSION=$(g++ -dumpversion | cut -d. -f1)
if [ "$CXX_VERSION" -lt 11 ]; then
    echo "g++ version $CXX_VERSION is too old. Requires g++ >= 11 for C++20 support."
    exit 1
fi

log "Building (C++20, -O3)..."

SRC_COMMON="src/graph_engine.cpp src/vertex.cpp src/message_bus.cpp \
            src/worker_node.cpp src/coordinator.cpp src/heartbeat.cpp \
            src/numa_scheduler.cpp src/order_book_graph.cpp \
            src/metrics.cpp src/rdma_transport.cpp"

FLAGS="-std=c++20 -O3 -march=native -Wall -Wextra -Wno-unused-parameter \
       -Iinclude -lpthread"

g++ $FLAGS src/main.cpp          $SRC_COMMON -o pregel_engine   2>&1
g++ $FLAGS src/bench_throughput.cpp $SRC_COMMON -o bench_throughput 2>&1

ok "Build complete: ./pregel_engine  ./bench_throughput"
echo ""

case "$MODE" in
    main)
        log "Running: pregel_engine --workers 4 --steps 20"
        echo ""
        ./pregel_engine --workers 4 --steps 20
        ;;
    bench)
        log "Running: bench_throughput (1→2→4 workers, 10 supersteps each)"
        echo ""
        ./bench_throughput
        ;;
    fault)
        log "Running: pregel_engine --workers 4 --steps 20 --fault"
        log "(Worker 1 will be killed via Phi Accrual FD at ~500ms)"
        echo ""
        ./pregel_engine --workers 4 --steps 20 --fault
        ;;
    *)
        echo "Unknown mode: $MODE. Run: bash run.sh help"
        exit 1
        ;;
esac
