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

#include <hpactor/runtime/actor_spawner.hpp>
#include <hpactor/runtime/cluster_runtime.hpp>
#include <hpactor/runtime/messaging_network_emitters.hpp>
#include <hpactor/runtime/messaging_runtime.hpp>
#include <hpactor/runtime/network_runtime.hpp>
#include <hpactor/runtime/observability_runtime.hpp>

#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/actor/lifecycle/shutdown_coordinator.hpp>
#include <hpactor/actor/request/ask_manager.hpp>
#include <hpactor/actor/spawn/actor_type_registry.hpp>
#include <hpactor/actor/stream/stream_registry.hpp>
#include <hpactor/actor/system/actor_directory.hpp>
#include <hpactor/actor/system/actor_system.hpp>
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
    metrics::MetricsActor* metrics_actor{nullptr};
};

/// \brief Stream protocol state (Phase 4 moves this into StreamRuntime).
struct StreamRuntimeState final {
    StreamRegistry registry;
    std::atomic<uint64_t> counter{0};
};

// ── ActorSystem::Impl ──────────────────────────────────────────────────────

class ActorSystem::Impl final : public ReliableAckTarget,
                                public BackpressureSignalTarget {
  public:
    Impl(ActorSystem& f, const Config& config);
    /// \brief Construct from a RuntimeBlueprint — construction only, no
    /// startup.
    Impl(ActorSystem& f, const class RuntimeBlueprint& bp);
    ~Impl();

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    ActorSystem& facade;

    // State groups in destruction order (last declared = first destroyed).
    // network_ is stopped explicitly before group destruction.
    CoreRuntimeState core;
    /// \brief Phase 7: sole observability resource owner (metrics, logging,
    ///        tracing, fault injection). Created before any producer.
    std::unique_ptr<ObservabilityRuntime> observability_;
    ActorServiceState actors;
    std::unique_ptr<MessagingRuntime> messaging_;
    StreamRuntimeState streams;
    /// \brief Fixed network-control output ports used by messaging.
    /// Constructed before messaging components; targets reach transport
    /// via impl->network_->transport().
    MessagingNetworkEmitters messaging_ports;
    /// \brief Phase 5: sole network resource owner.
    /// Null when networking is disabled.
    std::unique_ptr<NetworkRuntime> network_;
    /// \brief Phase 7: typed optional cluster runtime.
    /// Null when cluster is disabled.
    std::unique_ptr<IClusterRuntime> cluster_;

    /// \brief Fallback RpcChannel used when networking is disabled.
    /// Created during construction with the system scheduler; transport
    /// will be null.  Callers must check enable_network before using.
    std::unique_ptr<RpcChannel> rpc_channel_;

    // Spawner — constructed after directory, scheduler, metrics, logger exist.
    // Uses std::optional for deferred initialization.
    std::optional<ActorSpawner> spawner;

    // ── ReliableAckTarget ────────────────────────────────────────────────

    void send_ack(const ActorAddress& target, const ActorAddress& acker,
                  uint64_t message_id, uint8_t status,
                  uint32_t retry_after_ms) noexcept override;

    // ── BackpressureSignalTarget ──────────────────────────────────────────

    bool send_signal(const ActorAddress& target,
                     const StreamBuffer& encoded) noexcept override;
};

} // namespace hpactor
