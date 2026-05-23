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

#include <hpactor/actor/quarantine_policy.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/tracing/trace_config.hpp>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::config {

// DispatchPolicy is defined in types/types.hpp -- config uses the same enum via
// hpactor::DispatchPolicy.
using DispatchPolicy = hpactor::DispatchPolicy;

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
    /// Per-actor quarantine and circuit breaker policy. Defaults to
    /// disabled — set \c enabled = true in TOML to activate.
    QuarantinePolicy quarantine;
    std::unordered_map<std::string, std::string> args;
};

// -----------------------------------------------------------------------------
// SystemMailboxDef — system-wide mailbox defaults from [system.mailbox]
// -----------------------------------------------------------------------------
struct SystemMailboxDef {
#define HPACTOR_MAILBOX_FIELD(name, type, toml, def) type name{def};
#include <hpactor/config/mailbox_fields.def>
#undef HPACTOR_MAILBOX_FIELD
};

// -----------------------------------------------------------------------------
// SystemDef — global system configuration from TOML [system] section
//
// Maps to hpactor::Config at bootstrap time. Defaults match Config struct
// defaults defined in actor_system.hpp.
// -----------------------------------------------------------------------------
struct SystemDef {
    std::string version;

// ── Shared system fields (generated from system_toml_fields.def) ──
#define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def) type name{def};
#include <hpactor/config/system_toml_fields.def>
#undef HPACTOR_SYSTEM_TOML_FIELD

    // ── SystemDef-only fields ──
    uint32_t default_mailbox_size{1024};
    bool use_coroutines{false};
    bool metrics_enabled{true};
    uint32_t metrics_ring_buffer_capacity{65536};
    std::string metrics_path{"/metrics"};
    hpactor::log::LogConfig logging;
    hpactor::cli::CliConfig cli;
    std::string default_drain_policy{"Drain"};
    uint32_t default_drain_timeout_ms{30000};
    uint32_t shutdown_ingress_timeout_ms{5000};
    uint32_t shutdown_cluster_leave_timeout_ms{10000};
    bool shutdown_force_after_timeout{true};
    SystemMailboxDef mailbox;
    hpactor::mailbox::DeadLetterConfig dead_letters;
    std::string discovery_backend;
    std::vector<std::string> imports;
    hpactor::tracing::TraceConfig tracing;
    /// System-level defaults for per-actor quarantine policies.
    /// Individual actor definitions in TOML can override these values.
    hpactor::QuarantinePolicy quarantine_defaults;
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
