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

#include <hpactor/log/log_category.hpp>

namespace hpactor::log {

[[nodiscard]] const char* to_string(LogCategory category) noexcept {
    switch (category) {
        case LogCategory::kActor:
            return "actor";
        case LogCategory::kActorState:
            return "actor_state";
        case LogCategory::kMailbox:
            return "mailbox";
        case LogCategory::kScheduler:
            return "scheduler";
        case LogCategory::kMemory:
            return "memory";
        case LogCategory::kRegistrar:
            return "registrar";
        case LogCategory::kDiscovery:
            return "discovery";
        case LogCategory::kNetwork:
            return "network";
        case LogCategory::kRpc:
            return "rpc";
        case LogCategory::kConfig:
            return "config";
        case LogCategory::kSupervision:
            return "supervision";
        case LogCategory::kCli:
            return "cli";
        case LogCategory::kHttp:
            return "http";
        case LogCategory::kUser:
            return "user";
        case LogCategory::kCount:
            return "count";
    }
    return "unknown";
}

[[nodiscard]] const char* to_string(LogEventId id) noexcept {
    switch (id) {
        case LogEventId::kActorSpawned:
            return "actor_spawned";
        case LogEventId::kActorTerminated:
            return "actor_terminated";
        case LogEventId::kActorStateTransfer:
            return "actor_state_transfer";
        case LogEventId::kActorLinkRejected:
            return "actor_link_rejected";
        case LogEventId::kMailboxDepthHigh:
            return "mailbox_depth_high";
        case LogEventId::kMemoryAlloc:
            return "memory_alloc";
        case LogEventId::kMemoryFree:
            return "memory_free";
        case LogEventId::kMemoryCorruption:
            return "memory_corruption";
        case LogEventId::kRegistrarRegister:
            return "registrar_register";
        case LogEventId::kRegistrarResolveMiss:
            return "registrar_resolve_miss";
        case LogEventId::kDiscoveryNodeJoined:
            return "discovery_node_joined";
        case LogEventId::kDiscoveryNodeDead:
            return "discovery_node_dead";
        case LogEventId::kNetworkFrameReceived:
            return "network_frame_received";
        case LogEventId::kNetworkFrameDecodeFailed:
            return "network_frame_decode_failed";
        case LogEventId::kSchedulerDispatch:
            return "scheduler_dispatch";
        case LogEventId::kSchedulerSteal:
            return "scheduler_steal";
    }
    return "unknown_event";
}

[[nodiscard]] result<LogCategory> parse_category(std::string_view value) noexcept {
    if (value == "actor")
        return result<LogCategory>::make(LogCategory::kActor);
    if (value == "actor_state")
        return result<LogCategory>::make(LogCategory::kActorState);
    if (value == "mailbox")
        return result<LogCategory>::make(LogCategory::kMailbox);
    if (value == "scheduler")
        return result<LogCategory>::make(LogCategory::kScheduler);
    if (value == "memory")
        return result<LogCategory>::make(LogCategory::kMemory);
    if (value == "registrar")
        return result<LogCategory>::make(LogCategory::kRegistrar);
    if (value == "discovery")
        return result<LogCategory>::make(LogCategory::kDiscovery);
    if (value == "network")
        return result<LogCategory>::make(LogCategory::kNetwork);
    if (value == "rpc")
        return result<LogCategory>::make(LogCategory::kRpc);
    if (value == "config")
        return result<LogCategory>::make(LogCategory::kConfig);
    if (value == "supervision")
        return result<LogCategory>::make(LogCategory::kSupervision);
    if (value == "cli")
        return result<LogCategory>::make(LogCategory::kCli);
    if (value == "http")
        return result<LogCategory>::make(LogCategory::kHttp);
    if (value == "user")
        return result<LogCategory>::make(LogCategory::kUser);
    return result<LogCategory>::make(error(errors::unknown, "unknown log "
                                                            "category"));
}

} // namespace hpactor::log
