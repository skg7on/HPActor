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

// X-Macro tables for LogCategory and LogEventId.
//
// Each table is invoked as HPACTOR_LOG_CATEGORIES(X) or HPACTOR_LOG_EVENTS(X)
// with a caller-defined macro X that receives the entries.
//
// kCount is intentionally outside HPACTOR_LOG_CATEGORIES — it is a sentinel,
// not a real emit category, and is handled explicitly by to_string / parse.

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define HPACTOR_LOG_CATEGORIES(X)                                              \
    X(kActor, "actor")                                                         \
    X(kActorState, "actor_state")                                              \
    X(kMailbox, "mailbox")                                                     \
    X(kScheduler, "scheduler")                                                 \
    X(kMemory, "memory")                                                       \
    X(kRegistrar, "registrar")                                                 \
    X(kDiscovery, "discovery")                                                 \
    X(kNetwork, "network")                                                     \
    X(kRpc, "rpc")                                                             \
    X(kConfig, "config")                                                       \
    X(kSupervision, "supervision")                                             \
    X(kCli, "cli")                                                             \
    X(kHttp, "http")                                                           \
    X(kUser, "user")

// Ranges: 1000-1099 actor, 1100-1199 mailbox, 1200-1299 memory,
//         1300-1399 registrar/discovery, 1400-1499 network, 1500-1599 scheduler
#define HPACTOR_LOG_EVENTS(X)                                                  \
    X(kActorSpawned, 1000, "actor_spawned")                                    \
    X(kActorTerminated, 1001, "actor_terminated")                              \
    X(kActorStateTransfer, 1002, "actor_state_transfer")                       \
    X(kActorLinkRejected, 1003, "actor_link_rejected")                         \
    X(kActorDrainStart, 1004, "actor_drain_start")                             \
    X(kActorDrainComplete, 1005, "actor_drain_complete")                       \
    X(kActorDrainTimeout, 1006, "actor_drain_timeout")                         \
    X(kShutdownPhaseTransition, 1007, "shutdown_phase_transition")             \
    X(kMailboxDepthHigh, 1100, "mailbox_depth_high")                           \
    X(kMailboxHighWatermark, 1101, "mailbox_high_watermark")                   \
    X(kMailboxLowWatermarkRecovered, 1102, "mailbox_low_watermark_recovered")  \
    X(kMailboxFull, 1103, "mailbox_full")                                      \
    X(kMailboxMessageRejected, 1104, "mailbox_message_rejected")               \
    X(kMailboxMessageDropped, 1105, "mailbox_message_dropped")                 \
    X(kMailboxOverflowRerouted, 1106, "mailbox_overflow_rerouted")             \
    X(kBackpressureSignalSent, 1107, "backpressure_signal_sent")               \
    X(kSystemReserveExhausted, 1108, "system_reserve_exhausted")               \
    X(kDeadLetterQueued, 1109, "dead_letter_queued")                           \
    X(kDeadLetterLost, 1110, "dead_letter_lost")                               \
    X(kMemoryAlloc, 1200, "memory_alloc")                                      \
    X(kMemoryFree, 1201, "memory_free")                                        \
    X(kMemoryCorruption, 1202, "memory_corruption")                            \
    X(kRegistrarRegister, 1300, "registrar_register")                          \
    X(kRegistrarResolveMiss, 1301, "registrar_resolve_miss")                   \
    X(kDiscoveryNodeJoined, 1302, "discovery_node_joined")                     \
    X(kDiscoveryNodeDead, 1303, "discovery_node_dead")                         \
    X(kNetworkFrameReceived, 1400, "network_frame_received")                   \
    X(kNetworkFrameDecodeFailed, 1401, "network_frame_decode_failed")          \
    X(kSchedulerDispatch, 1500, "scheduler_dispatch")                          \
    X(kSchedulerSteal, 1501, "scheduler_steal")
// NOLINTEND(cppcoreguidelines-macro-usage)
