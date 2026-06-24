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

#include <hpactor/actor/lifecycle/quarantine_policy.hpp>
#include <hpactor/cli/config/cli_config.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/delivery_mode.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/net/endpoint_circuit_breaker.hpp>
#include <hpactor/net/endpoint_outbound_queue.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/tracing/trace_config.hpp>

#if HPACTOR_ENABLE_AI_ACCELERATORS
#    include <hpactor/ai/accelerator_config.hpp>
#endif

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::config {

/// \brief Re-export of hpactor::DispatchPolicy for config-layer convenience.
using DispatchPolicy = hpactor::DispatchPolicy;

/// \brief Per-actor memory resource specification.
struct ResourceSpec {
    /// \brief Size class in bytes for slab allocation. 0 = use default.
    uint32_t slab_class_bytes{0};
    /// \brief Maximum memory in KB for this actor. 0 = unlimited.
    uint32_t max_memory_kb{0};
};

/// \brief Defines a named scheduler thread pool (dispatcher).
struct DispatcherDef {
    /// \brief Unique dispatcher name, referenced by ActorDef::dispatcher.
    std::string name;
    /// \brief Number of worker threads in this pool.
    uint16_t threads{1};
    /// \brief CPU affinity mask. Empty = no pinning.
    std::vector<uint8_t> cpu_affinity;
};

/// \brief Per-actor mailbox overflow policy configuration.
struct MailboxPolicyDef {
    /// \brief Overflow policy for the mailbox.
    hpactor::mailbox::OverflowPolicy policy =
        hpactor::mailbox::OverflowPolicy::RejectNewest;
    /// \brief Enable priority-aware multi-lane routing.
    bool priority_aware{false};
    /// \brief Number of priority levels when priority_aware is enabled.
    uint8_t priority_levels{4};
    /// \brief Maximum depth of the overflow queue (0 = unlimited).
    uint32_t max_overflow_depth{0};
    /// \brief High watermark ratio for entering the High pressure state.
    double high_watermark{0.0};
    /// \brief Low watermark ratio for leaving the High pressure state.
    double low_watermark{0.0};
    /// \brief Critical watermark ratio for entering the Critical pressure
    /// state.
    double critical_watermark{0.0};
    /// \brief Minimum interval in ms between backpressure signal emissions.
    uint32_t signal_min_interval_ms{0};
    /// \brief Backpressure signal mode.
    hpactor::mailbox::BackpressureMode backpressure_mode{
        hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal};
};

/// \brief Defines a single actor instance within the topology.
struct ActorDef {
    /// \brief Unique actor identifier within the topology.
    std::string id;
    /// \brief Behavior name for factory lookup.
    std::string behavior;
    /// \brief Supervisor actor id (empty = unsupervised).
    std::string supervisor;
    /// \brief Dispatcher pool name (empty = default pool).
    std::string dispatcher;
    /// \brief Scheduling dispatch policy for this actor.
    DispatchPolicy dispatch_policy{DispatchPolicy::Cooperative};
    /// \brief Mailbox message capacity (0 = use system default).
    uint32_t mailbox_capacity{0};
    /// \brief Memory resource limits.
    ResourceSpec resources;
    /// \brief Mailbox overflow policy configuration.
    MailboxPolicyDef mailbox;
    /// \brief Per-actor quarantine and circuit breaker policy. Defaults to
    ///        disabled — set \c enabled = true in TOML to activate.
    QuarantinePolicy quarantine;
    /// \brief Key-value arguments passed to the actor's configure_from_args().
    std::unordered_map<std::string, std::string> args;
};

/// \brief System-wide mailbox defaults parsed from [system.mailbox].
///
/// Fields are generated from the mailbox_fields.def X-macro table.
struct SystemMailboxDef {
#define HPACTOR_MAILBOX_FIELD(name, type, toml, def) type name{def};
#include <hpactor/config/mailbox_fields.def>
#undef HPACTOR_MAILBOX_FIELD
};

/// \brief System-wide delivery defaults from [system.delivery].
struct DeliveryConfig {
    /// \brief Default delivery mode for actors that don't specify one.
    hpactor::mailbox::DeliveryMode default_mode =
        hpactor::mailbox::DeliveryMode::BestEffort;
    /// \brief Maximum retry count for at-least-once delivery.
    uint32_t max_retries = 3;
    /// \brief Initial retry backoff in ms.
    uint32_t retry_backoff_ms = 100;
    /// \brief Maximum retry backoff in ms (exponential backoff cap).
    uint32_t retry_backoff_max_ms = 10000;
    /// \brief Deduplication window in ms for tracked delivery.
    uint32_t dedup_window_ms = 300000;
    /// \brief Maximum dedup cache entries.
    uint32_t dedup_max_entries = 65536;
    /// \brief Default message TTL in milliseconds (0 = disabled).
    ///        Messages without an explicit deadline inherit this TTL.
    uint32_t default_message_ttl_ms = 0;
};

/// \brief Global system configuration from TOML [system] section.
///
/// Maps to hpactor::Config at bootstrap time. Defaults match Config struct
/// defaults defined in actor_system.hpp.
struct SystemDef {
    /// \brief Topology version string.
    std::string version;

// ── Shared system fields (generated from system_toml_fields.def) ──
#define HPACTOR_SYSTEM_TOML_FIELD(name, type, toml, def) type name{def};
#include <hpactor/config/system_toml_fields.def>
#undef HPACTOR_SYSTEM_TOML_FIELD

    /// \brief Default mailbox size for actors that don't specify one.
    uint32_t default_mailbox_size{1024};
    /// \brief Enable C++20 coroutine-based actors.
    bool use_coroutines{false};
    /// \brief Enable actor metrics collection.
    bool metrics_enabled{true};
    /// \brief Capacity of the metrics ring buffer.
    uint32_t metrics_ring_buffer_capacity{65536};
    /// \brief HTTP path for the Prometheus /metrics endpoint.
    std::string metrics_path{"/metrics"};
    /// \brief Structured logging configuration.
    hpactor::log::LogConfig logging;
    /// \brief Interactive CLI configuration.
    hpactor::cli::CliConfig cli;
    /// \brief Default drain policy for graceful shutdown.
    std::string default_drain_policy{"Drain"};
    /// \brief Default drain timeout in ms.
    uint32_t default_drain_timeout_ms{30000};
    /// \brief Timeout in ms for ingress to stop accepting new connections.
    uint32_t shutdown_ingress_timeout_ms{5000};
    /// \brief Timeout in ms for cluster-leave protocol.
    uint32_t shutdown_cluster_leave_timeout_ms{10000};
    /// \brief Force shutdown after timeout even if drain is incomplete.
    bool shutdown_force_after_timeout{true};
    /// \brief System-wide mailbox defaults.
    SystemMailboxDef mailbox;
    /// \brief Dead-letter queue configuration.
    hpactor::mailbox::DeadLetterConfig dead_letters;
    /// \brief Delivery semantics configuration.
    DeliveryConfig delivery;
    /// \brief Service discovery backend name.
    std::string discovery_backend;
    /// \brief TOML import paths (glob patterns).
    std::vector<std::string> imports;
    /// \brief Distributed tracing configuration.
    hpactor::tracing::TraceConfig tracing;
    /// \brief System-level defaults for per-actor quarantine policies.
    ///        Individual actor definitions in TOML can override these values.
    hpactor::QuarantinePolicy quarantine_defaults;

#if HPACTOR_ENABLE_AI_ACCELERATORS
    /// \brief AI accelerator resource plane configuration.
    hpactor::ai::AcceleratorConfig ai_accelerators;
#endif

    /// \brief Per-endpoint outbound queue limits from
    /// [system.transport.outbound].
    hpactor::net::EndpointOutboundLimits transport_outbound_limits;
    /// \brief Endpoint circuit breaker config from [system.transport.outbound].
    hpactor::net::EndpointCircuitBreakerConfig transport_circuit_breaker;
    /// \brief Process configuration (mode, pidfile, stdio, working directory).
    process::ProcessConfig process;
};

/// \brief The complete, validated, topologically sorted topology model.
///
/// Produced by TomlParser::parse() and consumed by BootstrapEngine.
struct TopologyModel {
    /// \brief Global system configuration.
    SystemDef system;
    /// \brief Dispatcher thread pool definitions.
    std::vector<DispatcherDef> dispatchers;
    /// \brief Actor definitions in topological (dependency) order.
    std::vector<ActorDef> actors;
};

} // namespace hpactor::config
