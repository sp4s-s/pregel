set -e

SRC="src/main.cpp src/graph_engine.cpp src/vertex.cpp src/message_bus.cpp \
     src/worker_node.cpp src/coordinator.cpp src/heartbeat.cpp \
     src/numa_scheduler.cpp src/order_book_graph.cpp src/metrics.cpp \
     src/rdma_transport.cpp"

BENCH="src/bench_throughput.cpp src/graph_engine.cpp src/vertex.cpp src/message_bus.cpp \
     src/worker_node.cpp src/coordinator.cpp src/heartbeat.cpp \
     src/numa_scheduler.cpp src/order_book_graph.cpp src/metrics.cpp \
     src/rdma_transport.cpp"

FLAGS="-std=c++20 -O3 -march=native -Wall -Wextra -Wno-unused-parameter \
       -Iinclude -lpthread"

echo "[build] Compiling pregel_engine..."
g++ $FLAGS $SRC -o pregel_engine

echo "[build] Compiling bench_throughput..."
g++ $FLAGS $BENCH -o bench_throughput

echo "[build] Done. Binaries: ./pregel_engine  ./bench_throughput"
