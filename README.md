# Distributed Graph Processing Engine (Pregel-Style)
## C++20 · RDMA-Simulated · NUMA-Aware · Phi Accrual Failure Detection

---

## Quick Start

```bash
git clone <repo> && cd pregel_engine
bash run.sh           
bash run.sh bench     
bash run.sh fault     
bash run.sh help      
```

Or manually:
```bash
g++ -std=c++20 -O3 -march=native -Iinclude -lpthread \
    src/main.cpp src/graph_engine.cpp src/vertex.cpp \
    src/message_bus.cpp src/worker_node.cpp src/coordinator.cpp \
    src/heartbeat.cpp src/numa_scheduler.cpp src/order_book_graph.cpp \
    src/metrics.cpp src/rdma_transport.cpp \
    -o pregel_engine

./pregel_engine --workers 4 --steps 20
./pregel_engine --workers 4 --steps 20 --fault     # with fault injection
```

**Requirements:** g++ >= 11, C++20 support, pthreads. No external dependencies.

---

## Architecture

### Computation Model

The engine implements Google's Pregel BSP (Bulk Synchronous Parallel) model.
Computation proceeds in discrete *supersteps*. In each superstep, every
vertex receives all messages sent to it in the prior superstep, runs its
`compute()` function, and sends messages to arbitrary vertices. At the end
of the superstep a global barrier synchronises all workers before the next
step begins.

A vertex that has no work to do calls `vote_to_halt()`. The computation
terminates when all vertices have halted *and* there are no messages
in flight — the same convergence condition as the original Pregel paper.

### Key Components

**GraphEngine** — top-level entry point. Wires up all subsystems and
delegates graph loading to the domain layer.

**WorkerNode** — owns a partition of the vertex set. Each superstep it
drains the message bus, dispatches messages to their target vertices,
schedules vertex computation via NumaScheduler, collects outgoing messages,
and routes them back through MessageBus. Workers run in parallel threads
(one `std::async` task per worker per superstep).

**Coordinator** — drives the global superstep loop, collects per-worker
statistics, and renders the live progress table. In a real MPI deployment
the coordinator is a dedicated rank (rank 0) that issues MPI barriers.

**MessageBus** — routes messages between workers. Local messages (same
worker) are placed directly into the inbox. Remote messages are handed
to RdmaTransport. A shared atomic barrier replaces `MPI_Barrier` in the
single-process simulation.

**RdmaTransport / QueuePair** — simulates one-sided RDMA WRITE semantics
with a configurable latency model (default: 2 µs per write, matching
RoCEv2/InfiniBand). Each worker maintains one QueuePair per remote worker.
A background thread per QP simulates the wire by delivering messages after
the latency window and posting completions to the CQ.

**HeartbeatDetector** — implements the Phi Accrual failure detector. Each
worker publishes periodic heartbeats; the detector tracks inter-arrival
times in a rolling window and computes a suspicion level φ. When φ exceeds
a configurable threshold (default: 8.0), the worker is marked FAILED and
the coordinator's failure callback fires to initiate partition handoff.

**NumaScheduler** — maintains per-NUMA-node task queues with thread pools
pinned to each node. Vertex tasks carry a `preferred_numa_node` hint derived
from their vertex ID, so that related vertices (and their memory) stay on
the same NUMA domain. The scheduler tracks actual vs. expected task duration;
tasks that exceed 3× the expected duration are classified as latency spikes
and logged for NUMA migration decisions.

**PartitionMap** — maps vertex IDs to owning workers. Default policy is
`vertex_id % worker_count`. Overrides (for migration after failure) are
stored in a concurrent hash map protected by a shared mutex.

---

## Domain: Order-Book Graph

The real-world workload models a multi-venue limit order book ecosystem
across 5 simulated exchanges (NYSE, NASDAQ, CBOE, BATS, IEX).

```
VenueVertex
  └─ PriceLevelVertex (bid/ask × 10 levels × 5 venues = 100 nodes)
       └─ OrderVertex  (20 orders/level × 100 levels = 2,000 nodes)

MarketMakerVertex (8 nodes, connected to all venues and sample price levels)
```

**Computation semantics:**

- *Superstep 0:* Venues broadcast their current NBBO (best bid/offer) with a synthetic random price walk to keep the graph live.
- *Superstep 1:* Price-level nodes aggregate fills and propagate PRICE_UPDATE messages downstream to orders and upstream to the venue.
- *Superstep 2:* Order nodes check if their limit price is now marketable. If so they emit ORDER_FILL messages to their parent price level and connected market maker.
- *Superstep 3:* Market-maker nodes update net position and PnL. If the observed bid from one venue exceeds the ask from another, they emit an ARBITRAGE_SIGNAL.
- *Halting:* Orders that have been fully filled or inactive for 10 supersteps vote to halt. Market makers vote to halt if their inbox and outbox are both empty. Venues never halt (they continuously drive the market).

**Message types:** ORDER_FILL, PRICE_UPDATE, CANCEL_ACK, POSITION_UPDATE, ARBITRAGE_SIGNAL, HEARTBEAT, BARRIER_SYNC.

**Fixed-point arithmetic:** All prices use 6-decimal fixed-point (int64_t × 1e6) to avoid floating-point non-determinism across workers.

---

## CLI Options

```
./pregel_engine [options]
  --workers N       Number of simulated workers      (default: 4)
  --steps N         Maximum supersteps                (default: 100)
  --numa-nodes N    NUMA topology node count          (default: 2)
  --fault           Enable fault injection — kills worker 1 at ~500ms
  --no-rdma         Disable RDMA simulation (pure in-process delivery)
  --help            Show usage
```

---

not the best but learning...
## Design Decisions vs. the Paper

| Pregel Paper | This Implementation |
|---|---|
| All-reduce for barrier | Localized atomic counter (per-superstep epoch flip) — avoids global synchronisation bottleneck |
| TCP/IP messaging | Simulated one-sided RDMA WRITE with 2µs latency model |
| Uniform worker failure | Phi Accrual (adaptive suspicion level, not fixed timeout) |
| Static NUMA assignment | Dynamic migration on latency spike detection |
| No partition overrides | Runtime PartitionMap overrides with shared_mutex |

-----------------------------
## Rests



 Each WorkerNode owns a subset of vertices and drives their BSP supersteps.
 Execution model per superstep:
   1. Receive messages from MessageBus (includes RDMA receives)
   2. Dispatch messages to target vertices
   3. Run compute() on all active (non-halted) vertices via NumaScheduler
   4. Collect outbox messages, route via MessageBus
   5. Vote to coordinator: #active vertices + #messages sent
   6. Wait on global barrier before advancing to next superstep

Fault tolerance:
-   If the heartbeat detector marks a worker as failed, the coordinator
-   assigns its vertices to survivors. WorkerNode::absorb_partition()
-   ingests migrated vertices mid-run.
