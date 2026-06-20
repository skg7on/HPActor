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
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/config/topology_model.hpp>
#include <hpactor/core/actor_registry.hpp>
#include <hpactor/core/mailbox.hpp>
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/hpactor_config.hpp>
#if HPACTOR_ENABLE_AI_ACCELERATORS
#    include <hpactor/ai/accelerator_config.hpp>
#endif
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
#include <hpactor/sched/dispatch_policy.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_manager.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
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

namespace log {
class LogManager;
class Logger;
} // namespace log

namespace cli {
class CliActor;
} // namespace cli

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
    Clock& clock() {
        return clock_;
    }

    // ── System actor ──────────────────────────────────────────────────────

    /// \brief The system pseudo-actor handle.
    Actor system_actor() {
        return system_actor_;
    }

    // ── Registry access ───────────────────────────────────────────────────

    /// \brief Mutable access to the actor registry.
    actor_registry& registry() {
        return registry_;
    }

    // ── Protobuf type registry ────────────────────────────────────────────

    /// \brief Registry mapping \c TypeTag to protobuf message types.
    ProtoTypeRegistry& proto_registry() {
        return proto_registry_;
    }
    const ProtoTypeRegistry& proto_registry() const {
        return proto_registry_;
    }

    // ── Node identity ─────────────────────────────────────────────────────

    /// \brief Network endpoint of this node.
    EndPoint endpoint() const {
        return endpoint_;
    }

    // ── Running state ─────────────────────────────────────────────────────

    /// \brief Returns \c true while the system is accepting messages.
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    /// \brief System uptime since construction.
    ///
    /// \return Elapsed time in milliseconds since \c ActorSystem construction.
    std::chrono::milliseconds uptime() const {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_time_);
    }

    /// \brief Read-only access to the system configuration.
    const Config& config() const {
        return config_;
    }

    // ── Scheduler ─────────────────────────────────────────────────────────

    /// \brief Pointer to the scheduler for direct scheduling operations.
    sched::IScheduler* scheduler() {
        return scheduler_.get();
    }

    /// \brief Returns \c true if coroutine-based execution is configured.
    ///
    /// Requires \c HPACTOR_SUPPORT_COROUTINES=1 at compile time.
    bool use_coroutines() const {
        return config_.use_coroutines;
    }

    // ── RPC ───────────────────────────────────────────────────────────────

    /// \brief Reference to the RPC channel for remote calls.
    RpcChannel& rpc_channel() {
        return *rpc_channel_;
    }

    // ── Ask ───────────────────────────────────────────────────────────────

    /// \brief AskManager for local ask() request tracking.
    AskManager* ask_manager() {
        return ask_manager_.get();
    }
    const AskManager* ask_manager() const {
        return ask_manager_.get();
    }

    /// \brief PassivationManager for actor passivation and reactivation.
    PassivationManager* passivation_manager() {
        return passivation_manager_.get();
    }
    const PassivationManager* passivation_manager() const {
        return passivation_manager_.get();
    }

    // ── HTTP ──────────────────────────────────────────────────────────────

    /// \brief Reference to the HTTP client for outbound requests.
    net::HttpClient& http_client() {
        return *http_client_;
    }

    // ── Distributed tracing ───────────────────────────────────────────────

    /// \brief Trace manager (nullptr if tracing is disabled).
    tracing::TraceManager* trace_manager() noexcept {
        return trace_manager_.get();
    }
    const tracing::TraceManager* trace_manager() const noexcept {
        return trace_manager_.get();
    }

    /// \brief Log manager (nullptr if logging is disabled).
    log::LogManager* log_manager() noexcept {
        return log_manager_.get();
    }
    const log::LogManager* log_manager() const noexcept {
        return log_manager_.get();
    }

    /// \brief Apply a new tracing configuration at runtime.
    void apply_tracing_config(const tracing::TraceConfig& config);

    // ── Fault injection ───────────────────────────────────────────────────

    fault::FaultController& fault_controller() noexcept {
        return fault_controller_;
    }
    const fault::FaultController& fault_controller() const noexcept {
        return fault_controller_;
    }

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
    auto* metrics_ring_buffer() const {
        return metrics_ring_buffer_.get();
    }

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
    mailbox::DeadLetterQueue* dead_letter_queue() noexcept {
        return dead_letters_.get();
    }
    const mailbox::DeadLetterQueue* dead_letter_queue() const noexcept {
        return dead_letters_.get();
    }

    /// \brief Access the outbound delivery tracker for at-least-once delivery.
    ///
    /// \return A pointer to the \c OutboundDeliveryTracker, or \c nullptr
    ///         if the tracker has not been initialized. The tracker is
    ///         always initialized when the \c ActorSystem is constructed.
    /// \note Thread safety: The returned pointer is stable for the lifetime
    ///       of the \c ActorSystem. Callers may cache it.
    msg::OutboundDeliveryTracker* outbound_tracker() noexcept {
        return outbound_tracker_.get();
    }

    // Receiver dedup cache for at-least-once delivery
    adt::DedupCache* dedup_cache() {
        return dedup_cache_.get();
    }
    const adt::DedupCache* dedup_cache() const {
        return dedup_cache_.get();
    }

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
    net::EventLoop* event_loop() {
        return network_loop_.get();
    }

    /// \brief Primary transport for remote messaging.
    ///
    /// Returns \c nullptr if networking is not enabled.
    net::Transport* transport() {
        return transport_.get();
    }

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
    net::UdpRegistrar* registrar() {
        return registrar_.get();
    }

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
    ActorTypeRegistry& actor_type_registry() {
        return *actor_type_registry_;
    }
    const ActorTypeRegistry& actor_type_registry() const {
        return *actor_type_registry_;
    }

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
    ShutdownCoordinator* shutdown_coordinator() const {
        return shutdown_coordinator_.get();
    }

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
    Config config_;
    EndPoint endpoint_;
    Clock clock_;
    actor_registry registry_;
    ActorDirectory actor_directory_;
    std::unique_ptr<LocalDeliveryEngine> local_delivery_engine_;
    std::unique_ptr<mailbox::DeliveryPipeline> delivery_pipeline_;
    std::unique_ptr<BackpressureCoordinator> backpressure_coordinator_;
    std::unique_ptr<ShutdownCoordinator> shutdown_coordinator_;
    std::unordered_map<ActorType, ActorTypeDef> actor_types_;
    Actor system_actor_;

    // Actor storage consolidated into actor_directory_ above.
    // Use actor_directory_.find() / find_actor() / find_mailbox() /
    // find_context() / insert() / snapshot() / size() / erase()
    // instead of the previous separate maps + mutexes + ID generator.

    // System start time for uptime tracking
    std::chrono::steady_clock::time_point start_time_;

    // Running flag for network thread loop
    std::atomic<bool> running_{true};

    // Shutdown state
    std::atomic<ShutdownPhase> shutdown_phase_{ShutdownPhase::Running};
    std::atomic<bool> is_ready_{true};

    // Scheduler
    std::unique_ptr<sched::IScheduler> scheduler_;

    // Network components (owned)
    std::unique_ptr<net::TcpTransport> transport_;
    BackpressureSignalWireSink backpressure_signal_wire_sink_for_test_;
    std::shared_ptr<net::UdpRegistrar> registrar_;
    std::shared_ptr<net::IServiceDiscovery> discovery_;
    std::shared_ptr<net::ActorLocationCache> location_cache_;
    uint64_t cache_purge_timer_ = 0;
    uint64_t retry_timer_ = 0;
    std::unique_ptr<net::EventLoop> network_loop_;
    std::thread network_thread_;

    // Actor type registry for remote spawning (owned via pointer to avoid
    // circular dep)
    std::unique_ptr<ActorTypeRegistry> actor_type_registry_;

    // RPC channel for remote calls (after transport_ creation)
    std::unique_ptr<RpcChannel> rpc_channel_;
    // Ask manager for local ask() request tracking
    std::unique_ptr<AskManager> ask_manager_;
    // Passivation manager for actor passivation and reactivation
    std::unique_ptr<PassivationManager> passivation_manager_;
    // HTTP client for outbound HTTP calls
    std::unique_ptr<net::HttpClient> http_client_;

    // HTTP gateway actor (DaemonActor, spawned when enable_http_gateway = true)
    Actor http_gateway_actor_{nullptr};

    // CLI actor (DaemonActor, spawned when cli.enabled = true)
    std::shared_ptr<cli::CliActor> cli_actor_;

    // Receptionist system actor (service-key-based actor discovery)
    std::shared_ptr<receptionist::Receptionist> receptionist_;

    // Metrics configuration, ring buffer, and actor
    metrics::MetricsConfig metrics_config_;
    std::shared_ptr<metrics::MpscRingBuffer<metrics::MetricEvent>> metrics_ring_buffer_;
    metrics::MetricsActor* metrics_actor_{nullptr};

    // Logging subsystem
    log::LogConfig logging_config_;
    std::unique_ptr<log::LogManager> log_manager_;
    log::Logger* logger_ = nullptr;

    // Dead-letter queue
    std::unique_ptr<mailbox::DeadLetterQueue> dead_letters_;

    // Outbound delivery tracker for at-least-once delivery
    std::unique_ptr<msg::OutboundDeliveryTracker> outbound_tracker_;

    // Receiver dedup cache for at-least-once delivery
    std::unique_ptr<adt::DedupCache> dedup_cache_;

    // Tracing subsystem
    tracing::TraceConfig tracing_config_;
    std::unique_ptr<tracing::TraceManager> trace_manager_;

    // Fault injection
    fault::FaultController fault_controller_;

    // Proto type registry for protobuf message serialization
    ProtoTypeRegistry proto_registry_;
};

// -----------------------------------------------------------------------------
// Template implementations
// -----------------------------------------------------------------------------

template <typename T, typename... Args>
Actor ActorSystem::spawn(Args&&... args) {
    ActorId id = actor_directory_.allocate_id();
    auto actor = std::make_shared<T>(nullptr, *this, std::forward<Args>(args)...);
    actor->set_address(ActorAddress(endpoint_, actor->type(), id, 0));
    if constexpr (requires { T::kActorTypeName; }) {
        actor->set_type_name(T::kActorTypeName);
    } else {
        actor->set_type_name("unknown");
    }

    auto mailbox_ptr = std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
        id, scheduler_.get(), mailbox_config_for_spawn());
    auto* mbox = mailbox_ptr.get();

    auto actor_ctx = std::make_shared<ActorContext>(Actor(actor), this);
    actor->set_context(actor_ctx.get());

    ActorDirectoryEntry entry;
    entry.actor = Actor(actor);
    entry.instance = actor;
    entry.mailbox = mailbox_ptr;
    entry.context = actor_ctx;
    actor_directory_.insert(std::move(entry));

    actor->set_scheduler(scheduler_.get());
    actor->set_mailbox(mbox);

    if (metrics_ring_buffer_) [[unlikely]] {
        mbox->set_metrics_ring_buffer(metrics_ring_buffer_.get());
        actor->set_metrics_ring_buffer(metrics_ring_buffer_.get());
    }

    if (logger_) [[unlikely]] {
        mbox->set_logger(logger_);
        actor->set_logger(logger_);
    }

    switch (actor->dispatch_policy()) {
        case sched::DispatchPolicy::Cooperative:
            scheduler_->notify_ready(id, 0, INT64_MAX);
            break;
        case sched::DispatchPolicy::DedicatedThread:
            scheduler_->register_dedicated_thread(
                id, actor->dispatch_hints().cpu_affinity);
            break;
        case sched::DispatchPolicy::DedicatedPool:
            scheduler_->register_dedicated_pool(id, actor->dispatch_hints().pool_size);
            break;
    }

    actor->on_activate();

    if (auto* lc = actor->as_lifecycle()) {
        lc->transition(LifecycleState::kActive);
    }

    HPACTOR_LOG_INFO(log::LogCategory::kActor, id,
                     static_cast<uint32_t>(log::LogEventId::kActorSpawned),
                     "actor spawned",
                     log::field_lit("type", actor->type_name().data()));

    if (metrics_ring_buffer_) [[unlikely]] {
        metrics::MetricEvent evt{};
        evt.actor_id = id;
        evt.event_type = metrics::MetricEventType::kActorSpawned;
        evt.value_hi = 1;
        metrics_ring_buffer_->try_push(evt);
    }

    return Actor(actor);
}

} // namespace hpactor
