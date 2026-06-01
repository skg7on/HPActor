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

#include <hpactor/types/failure_envelope.hpp>
#include <hpactor/types/failure_reason.hpp>

#include <chrono>

namespace hpactor {

const char* to_string(FailureReason reason) noexcept {
    switch (reason) {
        case FailureReason::NoRoute:
            return "no_route";
        case FailureReason::NodeUnavailable:
            return "node_unavailable";
        case FailureReason::ActorDead:
            return "actor_dead";
        case FailureReason::ActorNotReady:
            return "actor_not_ready";
        case FailureReason::Quarantined:
            return "quarantined";
        case FailureReason::CircuitOpen:
            return "circuit_open";
        case FailureReason::MailboxFull:
            return "mailbox_full";
        case FailureReason::OutboundQueueFull:
            return "outbound_queue_full";
        case FailureReason::MemoryPressure:
            return "memory_pressure";
        case FailureReason::ResourceExhausted:
            return "resource_exhausted";
        case FailureReason::RemoteUnavailable:
            return "remote_unavailable";
        case FailureReason::Expired:
            return "expired";
        case FailureReason::Timeout:
            return "timeout";
        case FailureReason::RejectedByPolicy:
            return "rejected_by_policy";
        case FailureReason::Dropped:
            return "dropped";
        case FailureReason::MailboxClosed:
            return "mailbox_closed";
        case FailureReason::SerializationError:
            return "serialization_error";
        case FailureReason::TransportError:
            return "transport_error";
        case FailureReason::FrameRejected:
            return "frame_rejected";
        case FailureReason::Duplicate:
            return "duplicate";
        case FailureReason::Draining:
            return "draining";
        case FailureReason::ShuttingDown:
            return "shutting_down";
        case FailureReason::RetryExhausted:
            return "retry_exhausted";
        case FailureReason::SpawnFailed:
            return "spawn_failed";
        case FailureReason::Unknown:
            return "unknown";
    }
    return "unknown";
}

const char* to_string(FailureSource source) noexcept {
    switch (source) {
        case FailureSource::ActorRuntime:
            return "actor_runtime";
        case FailureSource::Mailbox:
            return "mailbox";
        case FailureSource::Rpc:
            return "rpc";
        case FailureSource::Transport:
            return "transport";
        case FailureSource::Discovery:
            return "discovery";
        case FailureSource::Scheduler:
            return "scheduler";
        case FailureSource::Config:
            return "config";
        case FailureSource::Security:
            return "security";
        case FailureSource::DurableStore:
            return "durable_store";
        case FailureSource::Supervision:
            return "supervision";
        case FailureSource::Cluster:
            return "cluster";
        case FailureSource::Unknown:
            return "unknown";
    }
    return "unknown";
}

FailureEnvelope
make_failure_envelope(FailureReason reason, ActorId actor_id,
                      const ActorAddress& sender, const ActorAddress& receiver,
                      MessageId message_id, const TraceContext& trace,
                      FailureSource source, std::string_view detail) noexcept {
    FailureEnvelope env;
    env.reason = reason;
    env.actor_id = actor_id;
    env.sender = sender;
    env.receiver = receiver;
    env.message_id = message_id;
    env.trace = trace;
    env.retryable = retryable(reason);
    env.source = source;
    if (!detail.empty()) {
        env.set_detail(detail);
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    env.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
    return env;
}

} // namespace hpactor
