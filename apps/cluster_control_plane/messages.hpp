// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace hpactor::apps::cluster_control_plane {

// =============================================================================
// TypeTag constants — range 0x00040000
// =============================================================================

inline constexpr TypeTag RateLimitCheckTag{0x00040000};
inline constexpr TypeTag RateLimitDecisionTag{0x00040001};
inline constexpr TypeTag PolicyUpdateTag{0x00040002};
inline constexpr TypeTag PolicyAckTag{0x00040003};
inline constexpr TypeTag ClusterStatusQueryTag{0x00040004};
inline constexpr TypeTag ClusterStatusReplyTag{0x00040005};
inline constexpr TypeTag ScenarioInjectTag{0x00040006};
inline constexpr TypeTag DrainSingletonTag{0x00040007};
inline constexpr TypeTag NodeReportTag{0x00040008};
inline constexpr TypeTag DeadLettersReportTag{0x00040009};
inline constexpr TypeTag MetricsTickTag{0x0004000A};
inline constexpr TypeTag ReliablePolicyUpdateTag{0x0004000B};

// =============================================================================
// Enums
// =============================================================================

enum class ScenarioKind : uint8_t {
    HappyPath = 0,
    NodeFailure,
    Partition,
    Recovery,
};

enum class RateLimitDecision : uint8_t {
    Allowed = 0,
    Denied,
};

enum class UpdateStatus : uint8_t {
    Acked = 0,
    Nacked,
    TimedOut,
};

// =============================================================================
// Payload structs
// =============================================================================

struct RateLimitCheckPayload {
    std::string tenant_id;
    uint32_t requested = 0;
    uint64_t timestamp_ns = 0;
};

struct RateLimitDecisionPayload {
    std::string tenant_id;
    RateLimitDecision decision = RateLimitDecision::Denied;
    uint32_t remaining_tokens = 0;
    uint32_t max_tokens = 0;
    uint32_t window_seconds = 0;
    uint64_t fencing_token = 0;
};

struct PolicyUpdatePayload {
    std::string tenant_id;
    uint32_t max_tokens = 100;
    uint32_t window_seconds = 1;
    uint64_t fencing_token = 0;
};

struct PolicyAckPayload {
    std::string tenant_id;
    uint64_t fencing_token = 0;
    UpdateStatus status = UpdateStatus::Acked;
    std::string reason;
};

struct ClusterStatusQueryPayload {
    bool include_shards = false;
    bool include_dlq = false;
};

struct ClusterStatusReplyPayload {
    std::string node_id;
    uint32_t alive_count = 0;
    uint32_t total_nodes = 0;
    std::string singleton_owner;
    std::string singleton_state;
    uint64_t fencing_token = 0;
    uint32_t dlq_depth = 0;
    uint32_t dlq_total_pushed = 0;
    uint32_t requests_processed = 0;
    uint32_t requests_allowed = 0;
    uint32_t requests_denied = 0;
    uint32_t reliable_acks = 0;
    uint32_t reliable_nacks = 0;
    uint32_t reliable_expired = 0;
};

struct ScenarioInjectPayload {
    ScenarioKind scenario = ScenarioKind::HappyPath;
    std::string target_node;
};

struct DrainSingletonPayload {
    std::string singleton_name;
    uint32_t timeout_ms = 5000;
};

struct NodeReportPayload {
    std::string node_id;
    uint8_t old_state = 0;
    uint8_t new_state = 0;
    std::string reason;
    bool routes_invalidated = false;
};

struct DeadLettersReportPayload {
    uint32_t depth = 0;
    uint32_t total_pushed = 0;
    uint32_t total_lost = 0;
};

struct MetricsTickPayload {
    uint32_t acks_received = 0;
    uint32_t nacks_received = 0;
    uint32_t expired_count = 0;
    uint32_t pending_count = 0;
};

// =============================================================================
// Binary serialization helpers
// =============================================================================

class BufferWriter {
  public:
    void u8(uint8_t value) {
        buffer_.push_back(value);
    }

    void u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void str(const std::string& value) {
        if (value.size() > std::numeric_limits<uint32_t>::max())
            return;
        u32(static_cast<uint32_t>(value.size()));
        auto begin = reinterpret_cast<const uint8_t*>(value.data());
        buffer_.insert(buffer_.end(), begin, begin + value.size());
    }

    StreamBuffer finish() {
        return std::move(buffer_);
    }

  private:
    StreamBuffer buffer_;
};

class BufferReader {
  public:
    explicit BufferReader(const StreamBuffer& buffer) : buffer_(buffer) {}

    bool u8(uint8_t& value) {
        if (offset_ + 1 > buffer_.size())
            return false;
        value = buffer_[offset_++];
        return true;
    }

    bool u32(uint32_t& value) {
        if (offset_ + 4 > buffer_.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
            value = (value << 8) | buffer_[offset_++];
        return true;
    }

    bool u64(uint64_t& value) {
        if (offset_ + 8 > buffer_.size())
            return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value = (value << 8) | buffer_[offset_++];
        return true;
    }

    bool str(std::string& value) {
        uint32_t size = 0;
        if (!u32(size))
            return false;
        if (offset_ + size > buffer_.size())
            return false;
        value.assign(reinterpret_cast<const char*>(buffer_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    bool done() const {
        return offset_ == buffer_.size();
    }

  private:
    const StreamBuffer& buffer_;
    size_t offset_ = 0;
};

// =============================================================================
// to_string helpers
// =============================================================================

inline const char* to_string(ScenarioKind value) {
    switch (value) {
        case ScenarioKind::HappyPath:
            return "happy-path";
        case ScenarioKind::NodeFailure:
            return "node-failure";
        case ScenarioKind::Partition:
            return "partition";
        case ScenarioKind::Recovery:
            return "recovery";
    }
    return "happy-path";
}

inline ScenarioKind scenario_from_string(std::string_view value) {
    if (value == "node-failure")
        return ScenarioKind::NodeFailure;
    if (value == "partition")
        return ScenarioKind::Partition;
    if (value == "recovery")
        return ScenarioKind::Recovery;
    return ScenarioKind::HappyPath;
}

inline const char* to_string(RateLimitDecision value) {
    switch (value) {
        case RateLimitDecision::Allowed:
            return "allowed";
        case RateLimitDecision::Denied:
            return "denied";
    }
    return "denied";
}

inline const char* to_string(UpdateStatus value) {
    switch (value) {
        case UpdateStatus::Acked:
            return "acked";
        case UpdateStatus::Nacked:
            return "nacked";
        case UpdateStatus::TimedOut:
            return "timed_out";
    }
    return "acked";
}

// ============================================================================
// Encoder / Decoder functions
// ============================================================================

inline StreamBuffer encode_rate_limit_check(const RateLimitCheckPayload& v) {
    BufferWriter w;
    w.str(v.tenant_id);
    w.u32(v.requested);
    w.u64(v.timestamp_ns);
    return w.finish();
}

inline bool
decode_rate_limit_check(const StreamBuffer& buf, RateLimitCheckPayload& v) {
    BufferReader r(buf);
    return r.str(v.tenant_id) && r.u32(v.requested) && r.u64(v.timestamp_ns) &&
           r.done();
}

inline StreamBuffer encode_rate_limit_decision(const RateLimitDecisionPayload& v) {
    BufferWriter w;
    w.str(v.tenant_id);
    w.u8(static_cast<uint8_t>(v.decision));
    w.u32(v.remaining_tokens);
    w.u32(v.max_tokens);
    w.u32(v.window_seconds);
    w.u64(v.fencing_token);
    return w.finish();
}

inline bool
decode_rate_limit_decision(const StreamBuffer& buf, RateLimitDecisionPayload& v) {
    BufferReader r(buf);
    uint8_t dec = 0;
    if (!r.str(v.tenant_id) || !r.u8(dec) || !r.u32(v.remaining_tokens) ||
        !r.u32(v.max_tokens) || !r.u32(v.window_seconds) ||
        !r.u64(v.fencing_token) || !r.done())
        return false;
    v.decision = static_cast<RateLimitDecision>(dec);
    return true;
}

inline StreamBuffer encode_policy_update(const PolicyUpdatePayload& v) {
    BufferWriter w;
    w.str(v.tenant_id);
    w.u32(v.max_tokens);
    w.u32(v.window_seconds);
    w.u64(v.fencing_token);
    return w.finish();
}

inline bool decode_policy_update(const StreamBuffer& buf, PolicyUpdatePayload& v) {
    BufferReader r(buf);
    return r.str(v.tenant_id) && r.u32(v.max_tokens) &&
           r.u32(v.window_seconds) && r.u64(v.fencing_token) && r.done();
}

inline StreamBuffer encode_policy_ack(const PolicyAckPayload& v) {
    BufferWriter w;
    w.str(v.tenant_id);
    w.u64(v.fencing_token);
    w.u8(static_cast<uint8_t>(v.status));
    w.str(v.reason);
    return w.finish();
}

inline bool decode_policy_ack(const StreamBuffer& buf, PolicyAckPayload& v) {
    BufferReader r(buf);
    uint8_t st = 0;
    if (!r.str(v.tenant_id) || !r.u64(v.fencing_token) || !r.u8(st) ||
        !r.str(v.reason) || !r.done())
        return false;
    v.status = static_cast<UpdateStatus>(st);
    return true;
}

inline StreamBuffer
encode_cluster_status_query(const ClusterStatusQueryPayload& v) {
    BufferWriter w;
    w.u8(v.include_shards ? 1 : 0);
    w.u8(v.include_dlq ? 1 : 0);
    return w.finish();
}

inline bool decode_cluster_status_query(const StreamBuffer& buf,
                                        ClusterStatusQueryPayload& v) {
    BufferReader r(buf);
    uint8_t sh = 0, dl = 0;
    if (!r.u8(sh) || !r.u8(dl) || !r.done())
        return false;
    v.include_shards = (sh != 0);
    v.include_dlq = (dl != 0);
    return true;
}

inline StreamBuffer
encode_cluster_status_reply(const ClusterStatusReplyPayload& v) {
    BufferWriter w;
    w.str(v.node_id);
    w.u32(v.alive_count);
    w.u32(v.total_nodes);
    w.str(v.singleton_owner);
    w.str(v.singleton_state);
    w.u64(v.fencing_token);
    w.u32(v.dlq_depth);
    w.u32(v.dlq_total_pushed);
    w.u32(v.requests_processed);
    w.u32(v.requests_allowed);
    w.u32(v.requests_denied);
    w.u32(v.reliable_acks);
    w.u32(v.reliable_nacks);
    w.u32(v.reliable_expired);
    return w.finish();
}

inline bool decode_cluster_status_reply(const StreamBuffer& buf,
                                        ClusterStatusReplyPayload& v) {
    BufferReader r(buf);
    return r.str(v.node_id) && r.u32(v.alive_count) && r.u32(v.total_nodes) &&
           r.str(v.singleton_owner) && r.str(v.singleton_state) &&
           r.u64(v.fencing_token) && r.u32(v.dlq_depth) &&
           r.u32(v.dlq_total_pushed) && r.u32(v.requests_processed) &&
           r.u32(v.requests_allowed) && r.u32(v.requests_denied) &&
           r.u32(v.reliable_acks) && r.u32(v.reliable_nacks) &&
           r.u32(v.reliable_expired) && r.done();
}

inline StreamBuffer encode_scenario_inject(const ScenarioInjectPayload& v) {
    BufferWriter w;
    w.u8(static_cast<uint8_t>(v.scenario));
    w.str(v.target_node);
    return w.finish();
}

inline bool
decode_scenario_inject(const StreamBuffer& buf, ScenarioInjectPayload& v) {
    BufferReader r(buf);
    uint8_t sc = 0;
    if (!r.u8(sc) || !r.str(v.target_node) || !r.done())
        return false;
    v.scenario = static_cast<ScenarioKind>(sc);
    return true;
}

inline StreamBuffer encode_drain_singleton(const DrainSingletonPayload& v) {
    BufferWriter w;
    w.str(v.singleton_name);
    w.u32(v.timeout_ms);
    return w.finish();
}

inline bool
decode_drain_singleton(const StreamBuffer& buf, DrainSingletonPayload& v) {
    BufferReader r(buf);
    return r.str(v.singleton_name) && r.u32(v.timeout_ms) && r.done();
}

inline StreamBuffer encode_node_report(const NodeReportPayload& v) {
    BufferWriter w;
    w.str(v.node_id);
    w.u8(v.old_state);
    w.u8(v.new_state);
    w.str(v.reason);
    w.u8(v.routes_invalidated ? 1 : 0);
    return w.finish();
}

inline bool decode_node_report(const StreamBuffer& buf, NodeReportPayload& v) {
    BufferReader r(buf);
    uint8_t ri = 0;
    if (!r.str(v.node_id) || !r.u8(v.old_state) || !r.u8(v.new_state) ||
        !r.str(v.reason) || !r.u8(ri) || !r.done())
        return false;
    v.routes_invalidated = (ri != 0);
    return true;
}

inline StreamBuffer encode_dead_letters_report(const DeadLettersReportPayload& v) {
    BufferWriter w;
    w.u32(v.depth);
    w.u32(v.total_pushed);
    w.u32(v.total_lost);
    return w.finish();
}

inline bool
decode_dead_letters_report(const StreamBuffer& buf, DeadLettersReportPayload& v) {
    BufferReader r(buf);
    return r.u32(v.depth) && r.u32(v.total_pushed) && r.u32(v.total_lost) &&
           r.done();
}

inline StreamBuffer encode_metrics_tick(const MetricsTickPayload& v) {
    BufferWriter w;
    w.u32(v.acks_received);
    w.u32(v.nacks_received);
    w.u32(v.expired_count);
    w.u32(v.pending_count);
    return w.finish();
}

inline bool decode_metrics_tick(const StreamBuffer& buf, MetricsTickPayload& v) {
    BufferReader r(buf);
    return r.u32(v.acks_received) && r.u32(v.nacks_received) &&
           r.u32(v.expired_count) && r.u32(v.pending_count) && r.done();
}

} // namespace hpactor::apps::cluster_control_plane
