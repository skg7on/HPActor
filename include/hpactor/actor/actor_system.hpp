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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/actor/lifecycle/drain_config.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/passivation_manager.hpp>
#include <hpactor/actor/lifecycle/shutdown_options.hpp>
#include <hpactor/actor/lifecycle/shutdown_phase.hpp>
#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/cli/config/cli_config.hpp>
#include <hpactor/config/topology_model.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/msg/proto_type_registry.hpp>
#if HPACTOR_ENABLE_AI_ACCELERATORS
#    include <hpactor/ai/accelerator_config.hpp>
#endif
#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_handle.hpp>
#include <hpactor/actor/stream_registry.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/delivery_pipeline.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/metrics/metrics_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/msg/request_timeout.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/tcp_transport.hpp>
#include <hpactor/process/process_config.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/sched/actor_execution_dependencies.hpp>
#include <hpactor/sched/dispatch_policy.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/timer/timer_stats_snapshot.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_manager.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace hpactor {

// Forward declarations
class ActorTypeRegistry;
class LocalDeliveryEngine;
class BackpressureCoordinator;
class ShutdownCoordinator;
namespace msg {
class OutboundDeliveryTracker;
}
namespace mailbox {
class OutboundTracker;
}

namespace log {
class LogManager;
class Logger;
} // namespace log

namespace cli {
class CliActor;
} // namespace cli

namespace cluster {
class ClusterFailureModel;
class RouteInvalidation;
} // namespace cluster
namespace cluster::singleton {
class SingletonManagerActor;
} // namespace cluster::singleton

namespace metrics {
class MetricsActor;
} // namespace metrics

namespace receptionist {
class Receptionist;
} // namespace receptionist

// Scheduler interface forward declaration
namespace sched {
class IScheduler;
class HybridScheduler;
} // namespace sched

/// \brief System-wide default mailbox configuration.
///
/// Fields are generated from \c config/mailbox_fields.def. Every spawned
/// actor inherits these defaults unless overridden by \c ActorDef.
struct MailboxDefaults {
#define HPACTOR_MAILBOX_FIELD(name, type, toml, def) type name{def};
#include <hpactor/config/mailbox_fields.def>
#undef HPACTOR_MAILBOX_FIELD
};

/// \brief System-wide configuration for \c ActorSystem.
///
/// Controls networking, scheduling, CLI, metrics, logging, tracing,
/// service discovery, mailbox defaults, and shutdown behavior. Construct
/// with desired overrides before passing to \c ActorSystem.
struct Config {
// ── Shared system fields (generated from config/system_fields.def) ──
#define HPACTOR_SYSTEM_FIELD(name, type, toml, def) type name{def};
#include <hpactor/config/system_fields.def>
#undef HPACTOR_SYSTEM_FIELD

    // ── Config-only fields ──

    /// \brief Local endpoint for this node.
    EndPoint endpoint = LocalEndpoint;

    /// \brief TLS configuration (used when \c enable_network is \c true).
    net::TlsConfig tls = {};

    /// \brief Connection pool configuration.
    net::PoolConfig pool = {};

    /// \brief Registrar configuration for service discovery.
    net::RegistrarConfig registrar = {};

    /// \brief Enable the HTTP client subsystem.
    ///        Requires \c enable_network = true.
    bool enable_http_client = false;

    /// \brief Enable the Receptionist subsystem for service-key-based
    ///        actor discovery. When \c false, the Receptionist actor is
    ///        not spawned and \c receptionist() returns \c nullptr.
    bool enable_receptionist = true;

    /// \brief Enable coroutine-based actor execution.
    ///
    /// Requires \c HPACTOR_SUPPORT_COROUTINES=1 at compile time.
    /// When \c false (default), actors use behavior-based scheduling.
    bool use_coroutines = false;

    /// \brief CLI subsystem configuration.
    cli::CliConfig cli;

    /// \brief Service discovery backend.
    ///
    /// \c nullptr (default) auto-selects based on \c enable_network and
    /// registrar config for backward compatibility.
    std::shared_ptr<net::IServiceDiscovery> service_discovery = nullptr;

    /// \brief Gossip protocol configuration.
    ///        Used when creating \c GossipMembership internally.
    net::GossipConfig gossip = {};

    /// \brief Mailbox defaults applied to every actor spawned via this system.
    MailboxDefaults mailbox;

    /// \brief Dead-letter queue configuration.
    mailbox::DeadLetterConfig dead_letters;

    /// \brief Default drain policy and timeout for actor shutdown.
    DrainConfig shutdown_drain{DrainPolicy::Drain,
                               std::chrono::milliseconds{30'000}};

    /// \brief Ingress drain timeout in milliseconds.
    uint32_t ingress_timeout_ms{5000};

    /// \brief Cluster leave timeout in milliseconds.
    uint32_t cluster_leave_timeout_ms{10000};

    /// \brief Force shutdown after all phase timeouts expire.
    bool shutdown_force_after_timeout{true};

    /// \brief Default message TTL in milliseconds.
    ///
    /// When non-zero, every message without an explicit deadline receives
    /// this TTL computed from the current monotonic clock. Zero (default)
    /// means no default TTL — messages without an explicit deadline are
    /// never expired by the runtime.
    std::chrono::milliseconds default_message_ttl_ms{0};

    /// \brief Timer backend selection.
    sched::TimerBackend timer_backend = sched::TimerBackend::TimingWheel;

    /// \brief Start scheduler workers in paused state.
    ///
    /// When \c true, workers are created but blocked until
    /// \c resume_workers() is called. Used for deterministic testing.
    bool scheduler_start_paused = false;

    /// \brief Distributed tracing configuration.
    tracing::TraceConfig tracing;

    /// \brief Process-mode configuration (foreground, daemon, systemd).
    process::ProcessConfig process;
#if HPACTOR_ENABLE_AI_ACCELERATORS
    /// \brief AI accelerator resource plane configuration.
    ///        Runtime-disabled by default; enable via [system.ai.accelerators].
    ai::AcceleratorConfig ai_accelerators{};
#endif
};

/// \brief Registration entry for an actor type.
struct ActorTypeDef {
    std::string name; ///< Human-readable type name.
    ActorType id;     ///< Numeric type tag.
};

/// \brief The actor runtime environment.
///
/// Owns the scheduler, transport, registry, service discovery, metrics,
/// logging, tracing, CLI, and dead-letter queue subsystems. Constructed
/// from a \c Config and runs until \c shutdown() is called.
///
/// \note Thread safety: Non-copyable, non-movable. Spawn/registry methods
///       are internally synchronized. Shutdown is coordinated via an
///       atomic phase state machine.
class ActorSystem {
  public:
    /// \brief Construct the actor system with the given configuration.
    ///
    /// Creates schedulers, networking, and all enabled subsystems.
    /// \param[in] config System configuration.
    explicit ActorSystem(const Config& config);

    /// \brief Preferred API: create a fully validated, ready system.
    ///
    /// Validates all config before any runtime side effect (threads,
    /// listeners, daemonization, actor spawns). Returns the system
    /// on success, or a typed error on validation/startup failure.
    static result<std::unique_ptr<ActorSystem>>
    create(const Config& config) noexcept;

    /// \brief Create with topology bootstrapping.
    ///
    /// Parses the TOML topology, validates actor factories, and spawns
    /// configured actors. All validation happens before startup.
    static result<std::unique_ptr<ActorSystem>>
    create(const Config& config, const std::string& topology_path) noexcept;

    /// \brief Shut down all subsystems in phase order.
    ~ActorSystem();

    // Non-copyable, non-movable
    ActorSystem(const ActorSystem&) = delete;
    ActorSystem& operator=(const ActorSystem&) = delete;
    ActorSystem(ActorSystem&&) = delete;
    ActorSystem& operator=(ActorSystem&&) = delete;

    // ── Actor spawning ────────────────────────────────────────────────────

    /// \brief Spawn an actor by type.
    ///
    /// \tparam T Actor subclass.
    /// \tparam Args Constructor argument types.
    /// \param[in] args Constructor arguments forwarded to the actor.
    /// \return An \c Actor handle to the spawned instance.
    template <typename T, typename... Args> Actor spawn(Args&&... args);

    /// \brief Spawn a pre-constructed actor with per-actor configuration.
    ///
    /// Used by \c BootstrapEngine for TOML-based topology bootstrapping.
    /// \param[in] actor Fully constructed actor instance.
    /// \param[in] def Actor definition from topology config.
    /// \return An \c Actor handle.
    Actor spawn_configured(std::shared_ptr<AbstractActor> actor,
                           const struct config::ActorDef& def);

    /// \brief Load and bootstrap actor topology from a TOML file.
    ///
    /// Convenience entry point: parses the TOML file, builds the topology
    /// model, and spawns all configured actors.
    /// \param[in] toml_path Path to the TOML configuration file.
    /// \return \c result<void> with error detail on parse or spawn failure.
    result<void> load_topology(const std::string& toml_path);

    // ── Actor registry ────────────────────────────────────────────────────

    /// \brief Register an actor by name for name-based resolution.
    void register_actor(const std::string& name, Actor actor);

    /// \brief Resolve a named actor.
    ///
    /// \param[in] name Actor name registered via \c register_actor().
    /// \return The resolved \c Actor, or an empty handle if not found.
    Actor resolve_actor(const std::string& name);

    /// \brief Unregister a named actor.
    void unregister_actor(const std::string& name);

    // ── Actor type registration ───────────────────────────────────────────

    /// \brief Register an actor type definition.
    void register_actor_type(const ActorTypeDef& def);

    /// \brief Look up an actor type definition.
    ///
    /// \param[in] type Numeric type tag.
    /// \return The matching \c ActorTypeDef.
    ActorTypeDef get_actor_type(ActorType type) const;

    // ── Clock ─────────────────────────────────────────────────────────────

    /// \brief Reference to the system monotonic clock.
    Clock& clock();

    // ── System actor ──────────────────────────────────────────────────────

    /// \brief The system pseudo-actor handle.
    Actor system_actor();

    // ── Registry access ───────────────────────────────────────────────────

    /// \brief Inline name→address registry (was a separate header; zero
    /// external consumers).
    class ActorRegistry {
      public:
        explicit ActorRegistry(ActorDirectory& directory)
            : directory_(directory) {}

        void put(const std::string& name, ActorAddress addr) {
            (void)directory_.register_name(name, std::move(addr));
        }

        ActorAddress get(const std::string& name) const {
            auto address = directory_.resolve_name(name);
            return address.has_value() ? address.value() : ActorAddress{};
        }

        void erase(const std::string& name) {
            (void)directory_.unregister_name(name);
        }

      private:
        ActorDirectory& directory_;
    };

    /// \brief Mutable access to the actor registry.
    ActorRegistry& registry();

    // ── Protobuf type registry ────────────────────────────────────────────

    /// \brief Registry mapping \c TypeTag to protobuf message types.
    ProtoTypeRegistry& proto_registry();
    const ProtoTypeRegistry& proto_registry() const;

    // ── Node identity ─────────────────────────────────────────────────────

    /// \brief Network endpoint of this node.
    EndPoint endpoint() const;

    // ── Running state ─────────────────────────────────────────────────────

    /// \brief Returns \c true while the system is accepting messages.
    bool is_running() const;

    /// \brief System uptime since construction.
    ///
    /// \return Elapsed time in milliseconds since \c ActorSystem construction.
    std::chrono::milliseconds uptime() const;

    /// \brief Read-only access to the system configuration.
    const Config& config() const;

    // ── Scheduler ─────────────────────────────────────────────────────────

    /// \brief Pointer to the scheduler for direct scheduling operations.
    sched::IScheduler* scheduler();

    /// \brief Returns \c true if coroutine-based execution is configured.
    ///
    /// Requires \c HPACTOR_SUPPORT_COROUTINES=1 at compile time.
    bool use_coroutines() const;

    /// \brief Collect a snapshot of timer statistics from the active backend.
    ///
    /// Delegates to \c HybridScheduler::timer_snapshot().  Returns an
    /// empty snapshot when the timer backend is not \c TimerPlane.
    sched::TimerStatsSnapshot timer_stats() const;

    // ── RPC ───────────────────────────────────────────────────────────────

    /// \brief Reference to the RPC channel for remote calls.
    RpcChannel& rpc_channel();

    // ── Ask ───────────────────────────────────────────────────────────────

    /// \brief AskManager for local ask() request tracking.
    AskManager* ask_manager();
    const AskManager* ask_manager() const;

    /// \brief PassivationManager for actor passivation and reactivation.
    PassivationManager* passivation_manager();
    const PassivationManager* passivation_manager() const;

    // ── HTTP ──────────────────────────────────────────────────────────────

    /// \brief Reference to the HTTP client for outbound requests.
    net::HttpClient& http_client();

    // ── Distributed tracing ───────────────────────────────────────────────

    /// \brief Trace manager (nullptr if tracing is disabled).
    tracing::TraceManager* trace_manager() noexcept;
    const tracing::TraceManager* trace_manager() const noexcept;

    /// \brief Log manager (nullptr if logging is disabled).
    log::LogManager* log_manager() noexcept;
    const log::LogManager* log_manager() const noexcept;

    /// \brief Apply a new tracing configuration at runtime.
    void apply_tracing_config(const tracing::TraceConfig& config);

    // ── Fault injection ───────────────────────────────────────────────────

    fault::FaultController& fault_controller() noexcept;
    const fault::FaultController& fault_controller() const noexcept;

    // ── Actor lookup ──────────────────────────────────────────────────────

    /// \brief Internal actor lookup by ID.
    ///
    /// Used by the scheduler to obtain the actor for message dispatch.
    /// \param[in] id Actor identifier.
    /// \return Shared pointer to the actor, or \c nullptr if not found.
    std::shared_ptr<AbstractActor> get_actor(ActorId id);

    // ── Metrics ───────────────────────────────────────────────────────────

    /// \brief Metrics ring buffer pointer.
    ///
    /// Returns \c nullptr if metrics are disabled.
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_ring_buffer() const;

    /// \brief MetricsActor instance.
    ///
    /// Returns \c nullptr if metrics are disabled or not yet spawned.
    metrics::MetricsActor* metrics_actor() const;

    // ── CLI ───────────────────────────────────────────────────────────────

    /// \brief CLI actor instance.
    ///
    /// Returns \c nullptr if CLI is disabled or not yet spawned.
    cli::CliActor* cli_actor() const;

    /// \brief Access the Receptionist system actor for service-key-based
    ///        actor discovery.
    ///
    /// \return Pointer to the Receptionist, or \c nullptr if not yet
    ///         spawned.
    receptionist::Receptionist* receptionist() const;

    // ── Cluster subsystem ──────────────────────────────────────────────────

    /// \brief Enable the cluster subsystem for this node.
    ///
    /// Creates ClusterFailureModel, SingletonManagerActor, and registers
    /// shard-coordinator as the first managed singleton. Wires observer
    /// callback for node state change to election re-run.
    /// \param[in] node_id This node's identifier in the cluster.
    void enable_cluster(const std::string& node_id);

    /// \brief Returns \c true when the cluster subsystem is enabled.
    bool cluster_enabled() const;

    /// \brief Cluster failure model (nullptr when cluster is disabled).
    cluster::ClusterFailureModel* cluster_failure_model();

    /// \brief Singleton manager actor wrapper (nullptr when cluster disabled).
    cluster::singleton::SingletonManagerActor* singleton_manager();

    /// \brief Route invalidation coordinator (nullptr when cluster disabled).
    cluster::RouteInvalidation* route_invalidation();

    // ── Mailbox ───────────────────────────────────────────────────────────

    /// \brief Get the mailbox for a specific actor.
    ///
    /// Used by the scheduler to access the actor's message queue.
    /// \param[in] id Actor identifier.
    /// \return Pointer to the mailbox, or \c nullptr if not found.
    mailbox::MPSCActorMailbox<TypedMessage>* get_mailbox(ActorId id);

    // ── Actor enumeration ─────────────────────────────────────────────────

    /// \brief Approximate count of live actors in this system.
    size_t actor_count() const;

    /// \brief Enumerate all actors.
    ///
    /// \param[in] callback Invoked with \c (ActorId, AbstractActor&) for
    ///                     each live actor. Must not spawn or kill actors
    ///                     (the internal lock is held).
    void for_each_actor(std::function<void(ActorId, AbstractActor&)> callback) const;

    // ── Message delivery ──────────────────────────────────────────────────

    /// \brief Deliver a message to a local actor.
    ///
    /// Enqueues onto the target's mailbox and notifies the scheduler.
    /// \param[in] target Actor ID.
    /// \param[in] msg Message to deliver (moved).
    void deliver_local(ActorId target, TypedMessage msg);

    /// \brief Deliver a message with priority and deadline.
    ///
    /// \param[in] target Actor ID.
    /// \param[in] msg Message to deliver.
    /// \param[in] priority 0–3 (0 = highest priority).
    /// \param[in] deadline_ns Absolute deadline in nanoseconds
    ///                       (\c INT64_MAX = no deadline).
    void deliver_local(ActorId target, TypedMessage msg, uint8_t priority,
                       int64_t deadline_ns);

    /// \brief Deliver a message to a local actor with EDF scheduling.
    ///
    /// The target actor's work item is placed in the scheduler's EDF queue
    /// and dispatched in earliest-deadline-first order relative to other
    /// EDF-scheduled actors.  Ordinary priority-only messages are unaffected.
    ///
    /// \param[in] target Actor ID to deliver to.
    /// \param[in] msg Message to deliver (moved into the pipeline).
    /// \param[in] deadline_ns Absolute delivery deadline in nanoseconds.
    ///                       Must not be \c INT64_MAX.
    /// \param[in] priority Priority level 0–3 (0 = highest).  Used as a
    ///                     tiebreaker within the same deadline bucket.
    void deliver_local_edf(ActorId target, TypedMessage msg,
                           int64_t deadline_ns, uint8_t priority = 0);

    /// \brief Bounded admission delivery — returns an \c EnqueueResult.
    ///
    /// \param[in] target Actor ID.
    /// \param[in] msg Message to deliver.
    /// \param[in] priority 0–3 (0 = highest).
    /// \param[in] deadline_ns Absolute delivery deadline.
    /// \param[in] options Delivery options (deadline, idempotency).
    /// \return \c EnqueueResult describing acceptance, rejection, or
    ///         actor-not-found.
    /// \retval ActorNotFound The target actor does not exist.
    /// \retval Rejected The target mailbox is at hard capacity.
    mailbox::EnqueueResult
    try_deliver_local(ActorId target, TypedMessage msg, uint8_t priority = 0,
                      int64_t deadline_ns = INT64_MAX,
                      mailbox::DeliveryOptions options = {});

    /// \brief Fast local delivery that bypasses the full DeliveryPipeline.
    ///
    /// Enqueues directly to the target mailbox without circuit breaker,
    /// TTL, dedup, or backpressure checks. Intended for internal benchmarks
    /// and hot paths where those checks are known to be unnecessary.
    ///
    /// \pre The target actor exists.
    /// \pre No circuit breaker, TTL, or dedup is needed.
    /// \param[in] target Actor ID to deliver to.
    /// \param[in] msg    Message to deliver (moved).
    /// \return \c EnqueueResult describing acceptance or rejection.
    /// \retval Accepted        Message was enqueued.
    /// \retval ActorNotFound   Target actor does not exist.
    mailbox::EnqueueResult try_deliver_local_fast(ActorId target, TypedMessage msg);

    /// \brief Deliver with a user-facing \c DeliveryResult.
    ///
    /// Wraps \c try_deliver_local() and converts the internal
    /// \c EnqueueResult to \c DeliveryResult for caller convenience.
    ///
    /// \param[in] target Actor ID.
    /// \param[in] msg Message to deliver.
    /// \param[in] priority 0–3 (0 = highest).
    /// \param[in] deadline_ns Absolute delivery deadline.
    /// \param[in] options Delivery options.
    /// \return \c DeliveryResult describing the delivery outcome.
    mailbox::DeliveryResult
    deliver_with_result(ActorId target, TypedMessage msg, uint8_t priority = 0,
                        int64_t deadline_ns = INT64_MAX,
                        mailbox::DeliveryOptions options = {});

    /// \brief Record a timeout against an actor for circuit breaker tracking.
    ///
    /// Callers should invoke this when a request to \p target times out
    /// (e.g., from ask/request timeout paths). If the target actor has
    /// quarantine enabled with a non-zero \c timeout_rate_threshold,
    /// the timeout is recorded in the failure-rate tracker and may trip
    /// the circuit breaker.
    ///
    /// \param[in] target The actor that timed out.
    /// \note Thread safety: safe to call from any thread. The target
    ///       actor's circuit breaker is accessed through the actor
    ///       registry; the timeout recording itself is single-writer
    ///       (scheduler thread only).
    void record_actor_timeout(ActorId target);

    // ── Dead-letter queue ─────────────────────────────────────────────────

    /// \brief Enqueue a dead-letter record.
    ///
    /// \param[in] record Dead-letter record to store.
    /// \return \c true if the record was accepted.
    bool dead_letter(mailbox::DeadLetterRecord record) noexcept;

    /// \brief Snapshot of current dead-letter queue contents.
    mailbox::DeadLetterQueueSnapshot dead_letter_snapshot() const noexcept;

    /// \brief Pop the oldest dead-letter record.
    ///
    /// \param[out] out Set to the popped record on success.
    /// \return \c true if a record was available.
    bool pop_dead_letter(mailbox::DeadLetterRecord& out) noexcept;

    /// \brief Direct access to the dead-letter queue.
    ///
    /// Returns nullptr if dead-letter queue is not initialized.
    mailbox::DeadLetterQueue* dead_letter_queue() noexcept;
    const mailbox::DeadLetterQueue* dead_letter_queue() const noexcept;

    /// \brief Access the outbound delivery tracker for at-least-once delivery.
    ///
    /// \return A pointer to the \c OutboundDeliveryTracker, or \c nullptr
    ///         if the tracker has not been initialized. The tracker is
    ///         always initialized when the \c ActorSystem is constructed.
    /// \note Thread safety: The returned pointer is stable for the lifetime
    ///       of the \c ActorSystem. Callers may cache it.
    msg::OutboundDeliveryTracker* outbound_tracker() noexcept;

    /// \brief Access the reliable messaging OutboundTracker.
    ///
    /// Tracks pending outbound messages with ACK/NACK/retry/expiry support.
    /// \return Pointer to the \c mailbox::OutboundTracker, or \c nullptr
    ///         if not yet initialized.
    mailbox::OutboundTracker* reliable_tracker() noexcept;
    const mailbox::OutboundTracker* reliable_tracker() const noexcept;

    /// \brief Send a reliable ACK/NACK frame back to a message sender.
    ///
    /// Constructs a \c WireFrame with the appropriate flags (\c AckRequested
    /// for ACK, \c AckResponse for NACK) and sends it via the transport.
    /// No-op when networking is disabled or transport is unavailable.
    ///
    /// \param[in] target The original sender to route the ACK back to.
    /// \param[in] acker  The local actor that is acknowledging.
    /// \param[in] msg_id The message ID being acknowledged.
    /// \param[in] status ACK status code (\c 0=Accepted, 1=Rejected,
    ///                   \c 2=Duplicate).
    /// \param[in] retry_after_ms Suggested retry delay for NACK (\c 0 for ACK).
    void
    send_reliable_ack(const ActorAddress& target, const ActorAddress& acker,
                      uint64_t msg_id, uint8_t status, uint32_t retry_after_ms);

    // Receiver dedup cache for at-least-once delivery
    adt::DedupCache* dedup_cache();
    const adt::DedupCache* dedup_cache() const;

    // Build a MailboxConfig from system-wide defaults in Config::mailbox.
    // ── Mailbox configuration ─────────────────────────────────────────────

    /// \brief Build a \c MailboxConfig from system-wide defaults.
    mailbox::MailboxConfig mailbox_config_for_spawn() const;

    /// \brief Build a \c MailboxConfig for an actor definition.
    ///
    /// Falls back to system defaults when \c ActorDef fields are zero.
    /// \param[in] def Actor definition from topology config.
    /// \return Merged mailbox configuration.
    mailbox::MailboxConfig
    mailbox_config_for_actor_def(const config::ActorDef& def) const;

    // ── Remote delivery ───────────────────────────────────────────────────

    /// \brief Deliver a remote message to the target actor's mailbox.
    ///
    /// Bridges the transport layer to the unified \c deliver_local() sink.
    /// \param[in] frame WireFrame containing the remote message.
    void deliver_remote(const net::WireFrame& frame);

    /// \brief Deliver a batch frame to local actors.
    void deliver_remote_batch(const net::WireFrame& frame);

    // ── Stream routing ───────────────────────────────────────────────────

    /// Register a stream sender for inbound ack routing.
    void register_stream_sender(uint64_t stream_id, ActorId actor_id);

    /// Register a stream receiver for inbound data routing.
    void register_stream_receiver(uint64_t stream_id, ActorId actor_id);

    /// Remove stream registrations on close/error.
    void unregister_stream(uint64_t stream_id);

    /// Allocate a unique stream ID from sender_id + monotonic counter.
    uint64_t allocate_stream_id(ActorId sender_id);

    /// Open a streaming session to a target actor.
    /// \return StreamHandle on success, std::nullopt if target unreachable.
    std::optional<StreamHandle>
    open_stream(ActorId target, StreamConfig config = {});

    // ── Node death ────────────────────────────────────────────────────────

    /// \brief Handle a remote node becoming unreachable.
    ///
    /// Called by \c IServiceDiscovery. Finds all actors linked to the dead
    /// endpoint and delivers \c DownMsg.
    /// \param[in] dead_ep Endpoint of the unreachable node.
    void on_node_dead(EndPoint dead_ep);

    // ── Backpressure ──────────────────────────────────────────────────────

    /// \brief Emit a backpressure signal to the sender actor.
    ///
    /// Delivered through the sender's \c ActorContext::handle_backpressure()
    /// handler.
    /// \param[in] signal Backpressure signal from a downstream mailbox.
    void signal_backpressure(const mailbox::BackpressureSignal& signal);

    void
    maybe_emit_backpressure_signal(mailbox::MPSCActorMailbox<TypedMessage>* mailbox,
                                   const mailbox::EnqueueResult& result,
                                   const mailbox::MailboxEnvelopeMeta& meta,
                                   bool emit_requested,
                                   mailbox::BackpressureMode backpressure_mode);

    void emit_local_backpressure_signal(const mailbox::BackpressureSignal& signal,
                                        mailbox::MailboxPressureState state);

    void emit_remote_backpressure_signal(const mailbox::BackpressureSignal& signal,
                                         mailbox::MailboxPressureState state);

    bool handle_remote_backpressure_signal(const net::WireFrame& frame);

    using BackpressureSignalWireSink =
        std::function<bool(const ActorAddress&, const StreamBuffer&)>;
    void
    set_backpressure_signal_wire_sink_for_test(BackpressureSignalWireSink sink);

    // ── I/O completion ────────────────────────────────────────────────────

    /// \brief Enqueue an I/O completion for delivery to an actor.
    ///
    /// Called by \c EventLoop when async I/O operations complete.
    /// \param[in] completion Completion event with actor target and result.
    void enqueue_completion(net::OpCompletion completion);

    // ── Network access ────────────────────────────────────────────────────

    /// \brief Access the network event loop.
    ///
    /// Returns \c nullptr if networking is not enabled.
    net::EventLoop* event_loop();

    /// \brief Primary transport for remote messaging.
    ///
    /// Returns \c nullptr if networking is not enabled.
    net::Transport* transport();

    /// \brief Get transport for a specific remote endpoint.
    ///
    /// Currently returns the single \c transport_ for all endpoints since
    /// \c TcpTransport handles per-endpoint routing internally via its
    /// pools map. The parameter is reserved for future multi-transport
    /// scenarios.
    /// \param[in] endpoint Remote endpoint (reserved).
    /// \return Transport instance, or \c nullptr if networking is disabled.
    net::Transport* get_transport_for(const EndPoint& endpoint);

    /// \brief UDP registrar for same-host service discovery.
    net::UdpRegistrar* registrar();

    // ── Remote spawn ──────────────────────────────────────────────────────

    /// \brief Synchronously spawn an actor on a remote node.
    ///
    /// Blocks until the remote spawn completes or fails.
    /// \param[in] node_name Remote node name.
    /// \param[in] actor_type Registered actor type name.
    /// \param[in] args Serialized constructor arguments.
    /// \param[in] timeout_override Per-call timeout override; uses system
    ///            default \c spawn_timeout_ms when default or zero.
    /// \return \c result<ActorRef> with the remote actor reference on
    ///         success, or an error.
    /// \note Thread safety: Safe from non-actor threads.
    result<ActorRef>
    spawn_remote(const std::string& node_name, const std::string& actor_type,
                 const StreamBuffer& args,
                 RequestTimeout timeout_override = RequestTimeout::use_default());

    /// \brief Asynchronously spawn an actor on a remote node.
    ///
    /// Returns immediately with a \c RequestHandle<ActorRef> that can be polled
    /// or blocked on. The spawn request is routed through \c RpcChannel for
    /// reliable delivery with retry and timeout.
    ///
    /// \param[in] node_name Remote node name.
    /// \param[in] actor_type Registered actor type name.
    /// \param[in] args Serialized constructor arguments.
    /// \param[in] timeout_override Per-call timeout override; uses system
    ///            default \c spawn_timeout_ms when default or zero.
    /// \return \c RequestHandle<ActorRef> handle for polling completion.
    RequestHandle<ActorRef> spawn_remote_async(
        const std::string& node_name, const std::string& actor_type,
        const StreamBuffer& args,
        RequestTimeout timeout_override = RequestTimeout::use_default());

    // ── Actor type registry ───────────────────────────────────────────────

    /// \brief Registry of spawnable actor types for remote spawning.
    ActorTypeRegistry& actor_type_registry();
    const ActorTypeRegistry& actor_type_registry() const;

    // ── Shutdown ──────────────────────────────────────────────────────────

    /// \brief Initiate node shutdown with default options.
    ///
    /// Drives the full phase machine: drains ingress, drains actors,
    /// leaves the cluster, flushes telemetry.
    /// \return \c result<void> with error detail on timeout.
    result<void> shutdown();

    /// \brief Initiate node shutdown with custom options.
    /// \param[in] opts Shutdown timeout and behavior options.
    /// \return \c result<void> with error detail on timeout.
    result<void> shutdown(const ShutdownOptions& opts);

    /// \brief Current phase of the shutdown state machine.
    ShutdownPhase shutdown_phase() const noexcept;

    /// \brief Access the ShutdownCoordinator for registering user-defined
    ///        shutdown phases.
    ShutdownCoordinator* shutdown_coordinator() const;

    // ── Health/readiness ──────────────────────────────────────────────────

    /// \brief Returns \c true when the system is ready to serve traffic.
    ///
    /// \c false during shutdown and before \c SystemInitTag is broadcast.
    bool is_ready() const noexcept;

    /// \brief Returns \c true while the system is draining.
    bool is_draining() const noexcept;

    // ── Per-actor drain config ────────────────────────────────────────────

    /// \brief Override the drain configuration for a specific actor.
    ///
    /// Used by CLI admin commands and runtime configuration reloads.
    /// \param[in] target Actor ID.
    /// \param[in] cfg New drain configuration.
    void set_drain_config(ActorId target, DrainConfig cfg);

  private:
    /// \brief Non-template adoption helper for spawn<T>().
    ///
    /// Preserves the current template-spawn algorithm exactly. Constructed
    /// by spawning code that has already created a concrete T and selected
    /// its type-name string. This is a migration seam — Phase 2 replaces
    /// it with ActorSpawner::adopt() and SpawnSpec.
    Actor adopt_preconstructed_actor(std::shared_ptr<AbstractActor> actor,
                                     std::string_view type_name);

    friend struct sched::ActorExecutionDependencies;
    friend class RuntimeBuilder;
    friend class RuntimeCoordinator;
    friend void register_runtime_startup_stages(class RuntimeCoordinator&,
                                                ActorSystem&, bool) noexcept;

    /// \brief Tag type for blueprint-based construction (no startup).
    struct FromBlueprint {};
    /// \brief Construct from a validated blueprint without starting components.
    /// Only accessible to RuntimeBuilder and RuntimeCoordinator (friends).
    ActorSystem(FromBlueprint, const class RuntimeBlueprint& bp);

    class Impl;
    std::unique_ptr<Impl> impl_;

    // Stream frame delivery
    void deliver_remote_stream_open(const net::WireFrame& frame);
    void deliver_remote_stream_data(const net::WireFrame& frame);
    void deliver_remote_stream_ack(const net::WireFrame& frame);
    void deliver_remote_stream_close(const net::WireFrame& frame);
    void deliver_remote_stream_error(const net::WireFrame& frame);

    // Actor storage consolidated into actor_directory_ above.
    // Use actor_directory_.find() / find_actor() / find_mailbox() /
    // find_context() / insert() / snapshot() / size() / erase()
    // instead of the previous separate maps + mutexes + ID generator.

    // System start time for uptime tracking

    // Running flag for network thread loop

    // Shutdown state

    // Scheduler

    // Network components — moved to impl_->network

    // Actor type registry for remote spawning (owned via pointer to avoid
    // circular dep)

    // RPC channel for remote calls — moved to impl_->network
    // Ask manager for local ask() request tracking
    // Passivation manager for actor passivation and reactivation
    // HTTP client for outbound HTTP calls

    // HTTP gateway actor (DaemonActor, spawned when enable_http_gateway = true)

    // CLI actor (DaemonActor, spawned when cli.enabled = true)

    // Receptionist system actor (service-key-based actor discovery)

    // Metrics configuration, ring buffer, and actor

    // Logging subsystem

    // Dead-letter queue owned by impl_->messaging.dead_letters

    // Outbound delivery tracker for at-least-once delivery
    std::unique_ptr<msg::OutboundDeliveryTracker> outbound_tracker_;

    // Reliable messaging outbound tracker (ACK/NACK/retry/expiry)
    std::unique_ptr<mailbox::OutboundTracker> reliable_tracker_;

    // Receiver dedup cache for at-least-once delivery
    std::unique_ptr<adt::DedupCache> dedup_cache_;

    // Tracing subsystem

    // Fault injection

    // Proto type registry for protobuf message serialization

    // Cluster subsystem (type-erased to avoid link cycle between
    // hpactor_lib ↔ hpactor_cluster).
    using cluster_cleanup_fn = void (*)(void*);
};

// -----------------------------------------------------------------------------
// Template implementations
// -----------------------------------------------------------------------------

template <typename T, typename... Args>
Actor ActorSystem::spawn(Args&&... args) {
    auto actor = std::make_shared<T>(nullptr, *this, std::forward<Args>(args)...);
    if constexpr (requires { T::kActorTypeName; }) {
        return adopt_preconstructed_actor(actor, T::kActorTypeName);
    }
    return adopt_preconstructed_actor(actor, "unknown");
}

} // namespace hpactor
