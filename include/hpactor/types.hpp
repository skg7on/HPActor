#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorId - unique identifier for an actor instance
// -----------------------------------------------------------------------------
struct ActorId {
    using counter_type = uint64_t;

    ActorId() = default;

    explicit ActorId(counter_type value) : value_(value) {}

    counter_type value() const { return value_; }

    bool operator==(const ActorId& other) const { return value_ == other.value_; }
    bool operator!=(const ActorId& other) const { return !(*this == other); }

private:
    counter_type value_ = 0;
};

// -----------------------------------------------------------------------------
// NodeId - identifier for a node in a distributed system
// -----------------------------------------------------------------------------
using NodeId = uint32_t;
constexpr NodeId InvalidNodeId = 0;

// -----------------------------------------------------------------------------
// ActorType - type identifier for an actor
// -----------------------------------------------------------------------------
using ActorType = uint32_t;
constexpr ActorType InvalidActorType = 0;

// -----------------------------------------------------------------------------
// incarnation_type - version counter for actor lifecycle
// -----------------------------------------------------------------------------
using incarnation_type = uint64_t;

// -----------------------------------------------------------------------------
// MessageId - unique identifier for a message
// -----------------------------------------------------------------------------
struct MessageId {
    using counter_type = uint64_t;

    MessageId() = default;

    explicit MessageId(counter_type value) : value_(value) {}

    counter_type value() const { return value_; }

    bool operator==(const MessageId& other) const { return value_ == other.value_; }
    bool operator!=(const MessageId& other) const { return !(*this == other); }

    static MessageId generate() { return MessageId(next_id_++); }

private:
    counter_type value_ = 0;
    static counter_type next_id_;
};

// Definition of static member
inline MessageId::counter_type MessageId::next_id_ = 1;

// -----------------------------------------------------------------------------
// error - error code wrapper (no exceptions in hot path)
// -----------------------------------------------------------------------------
class error {
public:
    error() = default;

    explicit error(uint32_t code, std::string msg = {})
        : code_(code), message_(std::move(msg)) {}

    uint32_t code() const { return code_; }
    const std::string& message() const { return message_; }

    bool ok() const { return code_ == 0; }
    explicit operator bool() const { return !ok(); }

private:
    uint32_t code_ = 0;
    std::string message_;
};

// -----------------------------------------------------------------------------
// errors namespace - canonical error codes
// -----------------------------------------------------------------------------
namespace errors {
constexpr uint32_t unknown = 1;
constexpr uint32_t actor_down = 2;
constexpr uint32_t actor_not_found = 3;
constexpr uint32_t mailbox_full = 4;
constexpr uint32_t timeout = 5;
constexpr uint32_t user = 1000;
} // namespace errors

// -----------------------------------------------------------------------------
// Clock - for time-based operations
// -----------------------------------------------------------------------------
using Clock = std::chrono::steady_clock;

// -----------------------------------------------------------------------------
// AlarmHandle - opaque handle for alarms
// -----------------------------------------------------------------------------
struct AlarmHandle {
    AlarmHandle() = default;

    explicit AlarmHandle(uint64_t id) : id_(id) {}

    uint64_t id() const { return id_; }

private:
    uint64_t id_ = 0;
};

// -----------------------------------------------------------------------------
// TraceContext - for distributed tracing
// -----------------------------------------------------------------------------
struct TraceContext {
    TraceContext() = default;

    TraceContext(uint64_t trace_id, uint64_t span_id, uint32_t flags)
        : trace_id_(trace_id), span_id_(span_id), flags_(flags) {}

    uint64_t trace_id() const { return trace_id_; }
    uint64_t span_id() const { return span_id_; }
    uint32_t flags() const { return flags_; }

private:
    uint64_t trace_id_ = 0;
    uint64_t span_id_ = 0;
    uint32_t flags_ = 0;
};

// -----------------------------------------------------------------------------
// bytes - byte buffer type
// -----------------------------------------------------------------------------
using bytes = std::vector<uint8_t>;

} // namespace hpactor