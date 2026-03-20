#pragma once
#include "types.hpp"
#include <vector>
#include <functional>
#include <optional>
#include <span>

namespace pregel {

struct Edge {
    VertexId   dst;
    int64_t    weight;      
    uint8_t    type;     
    uint8_t    pad[7];
};

class Vertex {
public:
    explicit Vertex(VertexId id, VertexType type, WorkerId owner)
        : id_(id), type_(type), owner_(owner) {}

    virtual ~Vertex() = default;

    virtual void compute(std::span<const Message> inbox, Superstep step) = 0;

    VertexId   id()     const { return id_; }
    VertexType type()   const { return type_; }
    WorkerId   owner()  const { return owner_; }
    bool       halted() const { return halted_; }

    void set_owner(WorkerId w) { owner_ = w; }
    void vote_to_halt()        { halted_ = true; }
    void wake_up()             { halted_ = false; }

    std::vector<Edge>&       edges()       { return edges_; }
    const std::vector<Edge>& edges() const { return edges_; }

    std::vector<Message>&    outbox()      { return outbox_; }
    void clear_outbox()                    { outbox_.clear(); }

protected:
    void send_message(VertexId dst, Message m) {
        m.src = id_;
        m.dst = dst;
        outbox_.push_back(m);
    }

    VertexId             id_;
    VertexType           type_;
    WorkerId             owner_;
    bool                 halted_{false};
    std::vector<Edge>    edges_;
    std::vector<Message> outbox_;
};

}