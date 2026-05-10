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

#include <cassert>
#include <cstring>
#include <hpactor/log/log_category.hpp>

int main() {
    using namespace hpactor::log;

    // Test to_string for LogCategory
    assert(std::strcmp(to_string(LogCategory::kActor), "actor") == 0);
    assert(std::strcmp(to_string(LogCategory::kActorState), "actor_state") == 0);
    assert(std::strcmp(to_string(LogCategory::kMailbox), "mailbox") == 0);
    assert(std::strcmp(to_string(LogCategory::kScheduler), "scheduler") == 0);
    assert(std::strcmp(to_string(LogCategory::kMemory), "memory") == 0);
    assert(std::strcmp(to_string(LogCategory::kRegistrar), "registrar") == 0);
    assert(std::strcmp(to_string(LogCategory::kDiscovery), "discovery") == 0);
    assert(std::strcmp(to_string(LogCategory::kNetwork), "network") == 0);
    assert(std::strcmp(to_string(LogCategory::kRpc), "rpc") == 0);
    assert(std::strcmp(to_string(LogCategory::kConfig), "config") == 0);
    assert(std::strcmp(to_string(LogCategory::kSupervision), "supervision") == 0);
    assert(std::strcmp(to_string(LogCategory::kCli), "cli") == 0);
    assert(std::strcmp(to_string(LogCategory::kHttp), "http") == 0);
    assert(std::strcmp(to_string(LogCategory::kUser), "user") == 0);

    // Test to_string for LogEventId (every defined event)
    assert(std::strcmp(to_string(LogEventId::kActorSpawned), "actor_spawned") == 0);
    assert(std::strcmp(to_string(LogEventId::kActorTerminated), "actor_"
                                                                "terminated") == 0);
    assert(std::strcmp(to_string(LogEventId::kActorStateTransfer), "actor_"
                                                                   "state_"
                                                                   "transfe"
                                                                   "r") == 0);
    assert(std::strcmp(to_string(LogEventId::kActorLinkRejected), "actor_link_"
                                                                  "rejected") == 0);
    assert(std::strcmp(to_string(LogEventId::kMailboxDepthHigh), "mailbox_"
                                                                 "depth_"
                                                                 "high") == 0);
    assert(std::strcmp(to_string(LogEventId::kMemoryAlloc), "memory_alloc") == 0);
    assert(std::strcmp(to_string(LogEventId::kMemoryFree), "memory_free") == 0);
    assert(std::strcmp(to_string(LogEventId::kMemoryCorruption), "memory_"
                                                                 "corruptio"
                                                                 "n") == 0);
    assert(std::strcmp(to_string(LogEventId::kRegistrarRegister), "registrar_"
                                                                  "register") == 0);
    assert(std::strcmp(to_string(LogEventId::kRegistrarResolveMiss), "registrar"
                                                                     "_resolve_"
                                                                     "miss") == 0);
    assert(std::strcmp(to_string(LogEventId::kDiscoveryNodeJoined), "discovery_"
                                                                    "node_"
                                                                    "joined") == 0);
    assert(std::strcmp(to_string(LogEventId::kDiscoveryNodeDead), "discovery_"
                                                                  "node_"
                                                                  "dead") == 0);
    assert(std::strcmp(to_string(LogEventId::kNetworkFrameReceived), "network_"
                                                                     "frame_"
                                                                     "receive"
                                                                     "d") == 0);
    assert(std::strcmp(to_string(LogEventId::kNetworkFrameDecodeFailed), "netwo"
                                                                         "rk_"
                                                                         "frame"
                                                                         "_deco"
                                                                         "de_"
                                                                         "faile"
                                                                         "d") == 0);
    assert(std::strcmp(to_string(LogEventId::kSchedulerDispatch), "scheduler_"
                                                                  "dispatch") == 0);
    assert(std::strcmp(to_string(LogEventId::kSchedulerSteal), "scheduler_"
                                                               "steal") == 0);

    // Test to_string for unknown LogEventId
    assert(std::strcmp(to_string(static_cast<LogEventId>(9999)), "unknown_"
                                                                 "event") == 0);

    // Test parse_category success
    assert(parse_category("actor").has_value());
    assert(parse_category("actor").value() == LogCategory::kActor);
    assert(parse_category("actor_state").value() == LogCategory::kActorState);
    assert(parse_category("mailbox").value() == LogCategory::kMailbox);
    assert(parse_category("scheduler").value() == LogCategory::kScheduler);
    assert(parse_category("memory").value() == LogCategory::kMemory);
    assert(parse_category("registrar").value() == LogCategory::kRegistrar);
    assert(parse_category("discovery").value() == LogCategory::kDiscovery);
    assert(parse_category("network").value() == LogCategory::kNetwork);
    assert(parse_category("rpc").value() == LogCategory::kRpc);
    assert(parse_category("config").value() == LogCategory::kConfig);
    assert(parse_category("supervision").value() == LogCategory::kSupervision);
    assert(parse_category("cli").value() == LogCategory::kCli);
    assert(parse_category("http").value() == LogCategory::kHttp);
    assert(parse_category("user").value() == LogCategory::kUser);

    // Test parse_category failure
    assert(!parse_category("invalid").has_value());

    return 0;
}
