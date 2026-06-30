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

#include "actor_spawner.hpp"
#include "messaging_network_ports.hpp"
#include "messaging_runtime.hpp"
#include <hpactor/runtime/network_runtime.hpp>

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/actor_type_registry.hpp>
#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
#include <hpactor/actor/stream_registry.hpp>
#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/cli/config/cli_config.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/backpressure_coordinator.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/mailbox/local_delivery_engine.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/metrics/metrics_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>
#include <hpactor/msg/proto_type_registry.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/event_loop.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/process/process_manager.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_manager.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace hpactor {

class ActorTypeRegistry;
class BackpressureCoordinator;
class LocalDeliveryEngine;
class ShutdownCoordinator;
namespace cli {
class CliActor;
}
namespace metrics {
class MetricsActor;
}
namespace receptionist {
class Receptionist;
}
namespace cluster {
class ClusterFailureModel;
class RouteInvalidation;
} // namespace cluster
namespace cluster::singleton {
class SingletonManagerActor;
}

// ── Named state groups ─────────────────────────────────────────────────────

struct CoreRuntimeState final {
    Config config;
    EndPoint endpoint;
    Clock clock;
    std::chrono::steady_clock::time_point start_time;
    std::atomic<bool> running{true};
    std::atomic<ShutdownPhase> shutdown_phase{ShutdownPhase::Running};
    std::atomic<bool> is_ready{true};
    std::unique_ptr<sched::IScheduler> scheduler;
    ProtoTypeRegistry proto_registry;
};

struct ActorServiceState final {
    ActorDirectory directory;
    ActorSystem::ActorRegistry registry{directory};
    std::unordered_map<ActorType, ActorTypeDef> types;
    Actor system_actor;
    std::unique_ptr<ActorTypeRegistry> type_registry;
    std::unique_ptr<AskManager> ask;
    std::unique_ptr<PassivationManager> passivation;
    std::unique_ptr<net::HttpClient> http_client;
    Actor http_gateway_actor{nullptr};
    std::shared_ptr<cli::CliActor> cli_actor;
    std::shared_ptr<receptionist::Receptionist> receptionist;
    std::unique_ptr<ShutdownCoordinator> shutdown_coordinator;
};

/// \brief Stream protocol state (Phase 4 moves this into StreamRuntime).
struct StreamRuntimeState final {
    StreamRegistry registry;
    std::atomic<uint64_t> counter{0};
};

struct NetworkRuntimeState final {
    /// \brief Fixed network-control output ports used by messaging.
    /// Constructed before messaging components; transport may be null.
    MessagingNetworkPorts messaging_ports;

    std::unique_ptr<net::TcpTransport> transport;
    ActorSystem::BackpressureSignalWireSink backpressure_signal_wire_sink_for_test;
    std::shared_ptr<net::UdpRegistrar> registrar;
    std::shared_ptr<net::IServiceDiscovery> discovery;
    std::shared_ptr<net::ActorLocationCache> location_cache;
    uint64_t cache_purge_timer{0};
    uint64_t retry_timer{0};
    std::unique_ptr<net::EventLoop> event_loop;
    std::thread network_thread;
    std::unique_ptr<RpcChannel> rpc_channel;
};

struct OperationsRuntimeState final {
    metrics::MetricsConfig metrics_config;
    std::shared_ptr<metrics::MpscRingBuffer<metrics::MetricEvent>> metrics_ring_buffer;
    metrics::MetricsActor* metrics_actor{nullptr};
    log::LogConfig logging_config;
    std::unique_ptr<log::LogManager> log_manager;
    log::Logger* logger{nullptr};
    tracing::TraceConfig tracing_config;
    std::unique_ptr<tracing::TraceManager> trace_manager;
    fault::FaultController fault_controller;
};

struct ClusterRuntimeState final {
    using cluster_cleanup_fn = void (*)(void*);
    bool enabled{false};
    std::unique_ptr<void, cluster_cleanup_fn> failure_model{nullptr, +[](void*) {}};
    std::unique_ptr<void, cluster_cleanup_fn> singleton_manager{nullptr,
                                                                +[](void*) {}};
    std::unique_ptr<void, cluster_cleanup_fn> route_invalidation{nullptr,
                                                                 +[](void*) {}};
};

// ── ActorSystem::Impl ──────────────────────────────────────────────────────

class ActorSystem::Impl final {
  public:
    Impl(ActorSystem& f, const Config& config);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ActorSystem& facade;

    // State groups in destruction order (last declared = first destroyed).
    // network_ is stopped explicitly before group destruction.
    CoreRuntimeState core;
    OperationsRuntimeState operations;
    ActorServiceState actors;
    std::unique_ptr<MessagingRuntime> messaging_;
    StreamRuntimeState streams;
    /// \brief Phase 5: sole network resource owner.
    /// Null when networking is disabled.
    std::unique_ptr<NetworkRuntime> network_;
    /// \brief Deprecated: retained temporarily for messaging port adapters.
    /// Will be removed once adapters are moved into NetworkRuntime.
    NetworkRuntimeState network;
    ClusterRuntimeState cluster;

    // Spawner — constructed after directory, scheduler, metrics, logger exist.
    // Uses std::optional for deferred initialization.
    std::optional<ActorSpawner> spawner;
};

} // namespace hpactor
