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
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/failure_envelope.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace hpactor::mailbox {

enum class DeadLetterReason : uint8_t {
    MailboxFull,
    MailboxClosed,
    ActorNotFound,
    ActorTerminated,
    MissingRoute,
    RemoteNodeUnreachable,
    NetworkPartition,
    TransportSendFailed,
    DecodeFailed,
    OverflowPolicy,
    NoDropRejected,
    DrainTimeout = 12,    // message dropped because drain deadline expired
    DrainPolicyDrop = 13, // message dropped by DropUserMessages policy
    Expired = 14,         // message deadline expired before delivery
};

enum class DeadLetterSource : uint8_t {
    LocalDelivery,
    RemoteDelivery,
    ActorProxy,
    Transport,
    MailboxAdmission,
    ServiceDiscovery,
    Replay,
};

/// Map DeadLetterReason to the canonical FailureReason.
[[nodiscard]] constexpr FailureReason
failure_reason(DeadLetterReason reason) noexcept {
    switch (reason) {
        case DeadLetterReason::MailboxFull:
            return FailureReason::MailboxFull;
        case DeadLetterReason::MailboxClosed:
            return FailureReason::MailboxClosed;
        case DeadLetterReason::ActorNotFound:
            return FailureReason::NoRoute;
        case DeadLetterReason::ActorTerminated:
            return FailureReason::ActorDead;
        case DeadLetterReason::MissingRoute:
            return FailureReason::NoRoute;
        case DeadLetterReason::RemoteNodeUnreachable:
            return FailureReason::NodeUnavailable;
        case DeadLetterReason::NetworkPartition:
            return FailureReason::NodeUnavailable;
        case DeadLetterReason::TransportSendFailed:
            return FailureReason::TransportError;
        case DeadLetterReason::DecodeFailed:
            return FailureReason::SerializationError;
        case DeadLetterReason::OverflowPolicy:
            return FailureReason::RejectedByPolicy;
        case DeadLetterReason::NoDropRejected:
            return FailureReason::RejectedByPolicy;
        case DeadLetterReason::DrainTimeout:
            return FailureReason::Timeout;
        case DeadLetterReason::DrainPolicyDrop:
            return FailureReason::Dropped;
        case DeadLetterReason::Expired:
            return FailureReason::Expired;
    }
    return FailureReason::Unknown;
}

/// Map DeadLetterSource to the canonical FailureSource.
[[nodiscard]] constexpr FailureSource
failure_source(DeadLetterSource source) noexcept {
    switch (source) {
        case DeadLetterSource::LocalDelivery:
            return FailureSource::ActorRuntime;
        case DeadLetterSource::RemoteDelivery:
            return FailureSource::Transport;
        case DeadLetterSource::ActorProxy:
            return FailureSource::ActorRuntime;
        case DeadLetterSource::Transport:
            return FailureSource::Transport;
        case DeadLetterSource::MailboxAdmission:
            return FailureSource::Mailbox;
        case DeadLetterSource::ServiceDiscovery:
            return FailureSource::Discovery;
        case DeadLetterSource::Replay:
            return FailureSource::ActorRuntime;
    }
    return FailureSource::Unknown;
}

enum class DeadLetterOverflowPolicy : uint8_t {
    DropOldestRecord,
    DropNewestRecord,
    MetadataOnly,
};

struct DeadLetterConfig {
    bool enabled = true;
    uint32_t capacity = 4096;
    uint64_t byte_capacity = 0;
    uint32_t max_payload_sample_bytes = 512;
    DeadLetterOverflowPolicy overflow_policy =
        DeadLetterOverflowPolicy::DropOldestRecord;
    bool store_payload = true;
    bool alert_on_first_failure = false;
    uint32_t alert_threshold_per_minute = 100;
};

struct DeadLetterRecord {
    DeadLetterReason reason = DeadLetterReason::ActorNotFound;
    DeadLetterSource source = DeadLetterSource::LocalDelivery;
    ActorAddress sender;
    ActorAddress target;
    TypeTag type_tag = TypeTag::Invalid;
    uint64_t message_id = 0;
    uint32_t frame_flags = 0;
    uint8_t priority = 0;
    int64_t deadline_ns = INT64_MAX;
    uint64_t trace_id_hi = 0;
    uint64_t trace_id_lo = 0;
    uint64_t span_id = 0;
    uint32_t payload_size = 0;
    StreamBuffer payload_sample;
    uint32_t mailbox_depth = 0;
    uint32_t mailbox_capacity = 0;
    uint64_t timestamp_ns = 0;

    /// Build a FailureEnvelope from this dead-letter record's fields.
    [[nodiscard]] FailureEnvelope to_failure_envelope() const noexcept {
        FailureEnvelope env;
        env.reason = failure_reason(reason);
        env.actor_id = target.id;
        env.sender = sender;
        env.receiver = target;
        env.message_id = MessageId{message_id};
        env.trace = TraceContext{}; // DLQ records don't yet carry full trace
                                    // context
        env.retryable = ::hpactor::retryable(env.reason);
        env.timestamp_ns = timestamp_ns;
        env.source = failure_source(source);
        return env;
    }
};

struct DeadLetterQueueSnapshot {
    uint32_t depth = 0;
    uint32_t capacity = 0;
    uint64_t total_pushed = 0;
    uint64_t total_popped = 0;
    uint64_t total_lost = 0;
};

class IDeadLetterSink {
  public:
    virtual ~IDeadLetterSink() = default;
    virtual void on_dead_letter(const DeadLetterRecord& record) noexcept = 0;
};

class DeadLetterQueue {
  public:
    explicit DeadLetterQueue(DeadLetterConfig config = {});

    bool try_push(DeadLetterRecord&& record) noexcept;
    bool try_pop(DeadLetterRecord& out) noexcept;
    const DeadLetterConfig& config() const noexcept {
        return config_;
    }

    std::vector<DeadLetterRecord> snapshot_records() const;

    bool try_pop_at(size_t index, DeadLetterRecord& out) noexcept;

    DeadLetterQueueSnapshot snapshot() const noexcept;

  private:
    void trim_payload(DeadLetterRecord& record) const;

    DeadLetterConfig config_;
    mutable std::mutex mutex_;
    std::deque<DeadLetterRecord> records_;
    uint64_t total_pushed_{0};
    uint64_t total_popped_{0};
    uint64_t total_lost_{0};
};

} // namespace hpactor::mailbox
