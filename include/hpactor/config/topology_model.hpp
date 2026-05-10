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

#include <hpactor/cli/cli_config.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::config {

// -----------------------------------------------------------------------------
// DispatchPolicy — config-facing dispatch policy enum
//
// Mirrors sched::DispatchPolicy but lives in the config namespace to avoid
// coupling config parsing to scheduler internals. Converted at the bootstrap
// engine boundary.
// -----------------------------------------------------------------------------
enum class DispatchPolicy : uint8_t {
    Cooperative = 0,
    DedicatedThread,
    DedicatedPool,
};

// -----------------------------------------------------------------------------
// ResourceSpec — per-actor memory resource specification
// -----------------------------------------------------------------------------
struct ResourceSpec {
    uint32_t slab_class_bytes{0};
    uint32_t max_memory_kb{0};
};

// -----------------------------------------------------------------------------
// DispatcherDef — defines a named scheduler thread pool
// -----------------------------------------------------------------------------
struct DispatcherDef {
    std::string name;
    uint16_t threads{1};
    std::vector<uint8_t> cpu_affinity;
};

// -----------------------------------------------------------------------------
// MailboxPolicyDef — per-actor mailbox overflow policy configuration
// -----------------------------------------------------------------------------
struct MailboxPolicyDef {
    hpactor::mailbox::OverflowPolicy policy =
        hpactor::mailbox::OverflowPolicy::RejectNewest;
    bool priority_aware{false};
    uint32_t max_overflow_depth{0};
};

// -----------------------------------------------------------------------------
// ActorDef — defines a single actor instance within the topology
// -----------------------------------------------------------------------------
struct ActorDef {
    std::string id;
    std::string behavior;
    std::string supervisor;
    std::string dispatcher;
    DispatchPolicy dispatch_policy{DispatchPolicy::Cooperative};
    uint32_t mailbox_capacity{0};
    ResourceSpec resources;
    MailboxPolicyDef mailbox;
    std::unordered_map<std::string, std::string> args;
};

// -----------------------------------------------------------------------------
// SystemMailboxDef — system-wide mailbox defaults from [system.mailbox]
// -----------------------------------------------------------------------------
struct SystemMailboxDef {
    uint32_t default_capacity{1024};
    uint64_t default_byte_capacity{0};
    hpactor::mailbox::OverflowPolicy default_policy =
        hpactor::mailbox::OverflowPolicy::RejectNewest;
    double high_watermark{0.80};
    double low_watermark{0.50};
    uint32_t protected_system_messages{32};
    hpactor::mailbox::BackpressureMode backpressure =
        hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal;
};

// -----------------------------------------------------------------------------
// SystemDef — global system configuration from TOML [system] section
//
// Maps to hpactor::Config at bootstrap time. Defaults match Config struct
// defaults defined in actor_system.hpp.
// -----------------------------------------------------------------------------
struct SystemDef {
    std::string version;
    uint32_t scheduler_threads{4};
    uint32_t max_queue_depth{1024};
    uint32_t default_mailbox_size{1024};
    bool enable_network{false};
    uint16_t tcp_port{0};
    uint32_t spawn_timeout_ms{5000};
    bool enable_http_gateway{false};
    std::string http_bind_host{"0.0.0.0"};
    uint16_t http_port{8080};
    uint32_t http_max_connections{1000};
    uint32_t http_max_request_size{1048576};
    uint32_t http_reply_timeout_ms{5000};
    bool use_coroutines{false};
    bool metrics_enabled{true};
    uint32_t metrics_ring_buffer_capacity{65536};
    std::string metrics_path{"/metrics"};
    hpactor::log::LogConfig logging;
    hpactor::cli::CliConfig cli;
    SystemMailboxDef mailbox;
    hpactor::mailbox::DeadLetterConfig dead_letters;
    std::string discovery_backend;
    std::vector<std::string> imports;
};

// -----------------------------------------------------------------------------
// TopologyModel — the complete, validated, topologically sorted topology
// -----------------------------------------------------------------------------
struct TopologyModel {
    SystemDef system;
    std::vector<DispatcherDef> dispatchers;
    std::vector<ActorDef> actors; // Topologically sorted after Phase 3
};

} // namespace hpactor::config
