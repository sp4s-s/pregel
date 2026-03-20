#pragma once
#include "types.hpp"
#include "coordinator.hpp"
#include "worker_node.hpp"
#include "message_bus.hpp"
#include "numa_scheduler.hpp"
#include "heartbeat.hpp"
#include "order_book_graph.hpp"
#include <memory>
#include <vector>

namespace pregel {


class GraphEngine {
public:
    explicit GraphEngine(EngineConfig config);
    ~GraphEngine();

    void run();

    double benchmark(uint32_t supersteps);

private:
    void  setup_infrastructure();
    void  load_graph();
    void  distribute_vertices(GraphData& data);

    EngineConfig  config_;

    std::shared_ptr<PartitionMap>               pmap_;
    std::shared_ptr<NumaScheduler>              scheduler_;
    std::vector<std::shared_ptr<MessageBus>>    buses_;
    std::vector<std::shared_ptr<WorkerNode>>    workers_;
    std::shared_ptr<HeartbeatDetector>          hb_;
    std::unique_ptr<Coordinator>                coordinator_;
};

} 
