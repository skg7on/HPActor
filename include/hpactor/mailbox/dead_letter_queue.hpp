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
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <deque>
#include <mutex>

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
