#pragma once
#include "types.hpp"
#include <vector>
#include <array>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <queue>
#include <memory>

namespace pregel {


struct RdmaBuffer {
    uint8_t*  ptr;
    size_t    size;
    uint32_t  lkey;   
    uint32_t  rkey;   
};

enum class RdmaOpType : uint8_t { WRITE = 0, READ = 1, SEND = 2 };

struct RdmaWorkRequest {
    RdmaOpType    op;
    WorkerId      remote_worker;
    uint64_t      remote_addr;
    uint64_t      local_addr;
    uint32_t      length;
    uint64_t      wr_id;       
    bool          signaled;    
};

struct RdmaCompletion {
    uint64_t wr_id;
    uint32_t bytes;
    bool     success;
};


class QueuePair {
public:
    QueuePair(WorkerId local, WorkerId remote, size_t buf_size_bytes);
    ~QueuePair();

    bool post_write(const void* src, size_t len, uint64_t wr_id);

    std::vector<RdmaCompletion> poll_cq(int max_completions = 64);

    void flush();

    WorkerId local_id()  const { return local_; }
    WorkerId remote_id() const { return remote_; }

    std::vector<Message> drain_received();

private:
    WorkerId local_;
    WorkerId remote_;
    std::vector<uint8_t>     send_buf_;
    std::vector<uint8_t>     recv_buf_;

    mutable std::mutex       mu_;
    std::condition_variable  cv_;
    std::queue<Message>      inbox_;
    std::atomic<uint64_t>    pending_wr_{0};
    std::atomic<uint64_t>    next_wr_id_{1};

    std::thread              net_thread_;
    std::atomic<bool>        stop_{false};

    struct PendingWrite {
        uint64_t wr_id;
        Message  msg;
        std::chrono::steady_clock::time_point deliver_at;
    };
    std::queue<PendingWrite> in_flight_;
    std::mutex               flight_mu_;
    std::vector<RdmaCompletion> cq_;
    std::mutex               cq_mu_;

    void network_loop();
    static constexpr uint32_t RDMA_LATENCY_US = 2; 
};

class RdmaTransport {
public:
    explicit RdmaTransport(WorkerId local_id, uint32_t worker_count,
                           size_t buf_size_kb = 256);

    bool send(WorkerId dst, const Message& msg);

    bool send_batch(WorkerId dst, std::span<const Message> msgs);

    std::vector<Message> receive_from(WorkerId src);

    std::vector<Message> receive_all();

    uint64_t poll_completions();

    uint64_t bytes_sent()     const { return bytes_sent_.load(); }
    uint64_t rdma_ops()       const { return rdma_ops_.load(); }

private:
    WorkerId   local_id_;
    uint32_t   worker_count_;
    std::vector<std::unique_ptr<QueuePair>> qps_;  

    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<uint64_t> rdma_ops_{0};
};

} 
