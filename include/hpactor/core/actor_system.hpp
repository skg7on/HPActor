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
#include <hpactor/actor/drain_config.hpp>
#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/cli/cli_config.hpp>
#include <hpactor/config/topology_model.hpp>
#include <hpactor/core/actor_registry.hpp>
#include <hpactor/core/mailbox.hpp>
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/dedup_cache.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/metrics/metrics_config.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/net/actor_location_cache.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/net/gossip_membership.hpp>
#include <hpactor/net/http_client.hpp>
#include <hpactor/net/registrar.hpp>
#include <hpactor/net/service_discovery.hpp>
#include <hpactor/net/static_discovery.hpp>
#include <hpactor/net/tcp_transport.hpp>
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
class AsyncActor;
class ActorTypeRegistry;

namespace log {
class LogManager;
class Logger;
} // namespace log

namespace cli {
class CliActor;
} // namespace cli

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

    /// \brief Timer backend selection.
    sched::TimerBackend timer_backend = sched::TimerBackend::TimingWheel;

    /// \brief Start scheduler workers in paused state.
    ///
    /// When \c true, workers are created but blocked until
    /// \c resume_workers() is called. Used for deterministic testing.
    bool scheduler_start_paused = false;

    /// \brief Distributed tracing configuration.
    tracing::TraceConfig tracing;
};

/// \brief Registration entry for an actor type.
struct ActorTypeDef {
    std::string name; ///< Human-readable type name.
    ActorType id;     ///< Numeric type tag.
};

/// \brief Phases of the node shutdown state machine.
enum class ShutdownPhase : uint8_t {
    Running,           ///< Normal operation.
    DrainingIngress,   ///< Refusing new external connections and messages.
    DrainingActors,    ///< Draining in-flight actor messages per policy.
    LeavingCluster,    ///< Notifying peers and handing off shards.
    FlushingTelemetry, ///< Flushing metrics, logs, and traces.
    Stopped,           ///< Clean shutdown complete.
    ForcedStop,        ///< Force-stopped after timeout.
};

/// \brief Options controlling the shutdown sequence.
struct ShutdownOptions {
    /// \brief Maximum time for ingress draining.
    std::chrono::milliseconds ingress_timeout{5'000};
    /// \brief Maximum time for actor message draining.
    std::chrono::milliseconds actor_drain_timeout{30'000};
    /// \brief Maximum time for cluster leave handshake.
    std::chrono::milliseconds cluster_leave_timeout{10'000};
    /// \brief Force shutdown after all phase timeouts expire.
    bool force_after_timeout{true};
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

    /// \brief Apply a new tracing configuration at runtime.
    void apply_tracing_config(const tracing::TraceConfig& config);

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

    // ── CLI ───────────────────────────────────────────────────────────────

    /// \brief CLI actor instance.
    ///
    /// Returns \c nullptr if CLI is disabled or not yet spawned.
    cli::CliActor* cli_actor() const;

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

    // Receiver dedup cache for at-least-once delivery
    mailbox::DedupCache* dedup_cache() { return dedup_cache_.get(); }
    const mailbox::DedupCache* dedup_cache() const { return dedup_cache_.get(); }

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

    // ── I/O completion ────────────────────────────────────────────────────

    /// \brief Enqueue an I/O completion for delivery to an actor.
    ///
    /// Called by \c EventLoop when async I/O operations complete.
    /// \param[in] completion Completion event with actor target and result.
    void enqueue_completion(net::OpCompletion completion);

    // ── Network access ────────────────────────────────────────────────────

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
    /// \return \c result<ActorRef> with the remote actor reference on
    ///         success, or an error.
    /// \note Thread safety: Safe from non-actor threads.
    result<ActorRef>
    spawn_remote(const std::string& node_name, const std::string& actor_type,
                 const StreamBuffer& args);

    /// \brief Asynchronously spawn an actor on a remote node.
    ///
    /// Returns immediately with an \c AsyncActor handle that can be polled.
    /// \param[in] node_name Remote node name.
    /// \param[in] actor_type Registered actor type name.
    /// \param[in] args Serialized constructor arguments.
    /// \return \c AsyncActor handle for polling completion.
    AsyncActor
    spawn_remote_async(const std::string& node_name,
                       const std::string& actor_type, const StreamBuffer& args);

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
    std::unordered_map<ActorType, ActorTypeDef> actor_types_;
    Actor system_actor_;

    // Actor registry - maps ActorId to actor instance
    std::unordered_map<ActorId, std::shared_ptr<AbstractActor>> actors_;
    mutable std::mutex actors_mutex_;

    // Actor mailboxes - maps ActorId to mailbox
    std::unordered_map<ActorId, std::unique_ptr<mailbox::MPSCActorMailbox<TypedMessage>>> mailboxes_;
    std::mutex mailboxes_mutex_;

    // Actor contexts - maps ActorId to context
    std::unordered_map<ActorId, std::unique_ptr<ActorContext>> actor_contexts_;
    std::mutex actor_contexts_mutex_;

    // Actor ID generator
    std::atomic<uint64_t> next_actor_id_{1};

    // Running flag for network thread loop
    std::atomic<bool> running_{true};

    // Shutdown state
    std::atomic<ShutdownPhase> shutdown_phase_{ShutdownPhase::Running};
    std::atomic<bool> is_ready_{true};

    // Scheduler
    std::unique_ptr<sched::IScheduler> scheduler_;

    // Network components (owned)
    std::unique_ptr<net::TcpTransport> transport_;
    std::shared_ptr<net::UdpRegistrar> registrar_;
    std::shared_ptr<net::IServiceDiscovery> discovery_;
    std::shared_ptr<net::ActorLocationCache> location_cache_;
    uint64_t cache_purge_timer_ = 0;
    std::unique_ptr<net::EventLoop> network_loop_;
    std::thread network_thread_;

    // Actor type registry for remote spawning (owned via pointer to avoid
    // circular dep)
    std::unique_ptr<ActorTypeRegistry> actor_type_registry_;

    // RPC channel for remote calls (after transport_ creation)
    std::unique_ptr<RpcChannel> rpc_channel_;
    // HTTP client for outbound HTTP calls
    std::unique_ptr<net::HttpClient> http_client_;

    // HTTP gateway actor (DaemonActor, spawned when enable_http_gateway = true)
    Actor http_gateway_actor_{nullptr};

    // CLI actor (DaemonActor, spawned when cli.enabled = true)
    std::shared_ptr<cli::CliActor> cli_actor_;

    // Metrics configuration and ring buffer
    metrics::MetricsConfig metrics_config_;
    std::shared_ptr<metrics::MpscRingBuffer<metrics::MetricEvent>> metrics_ring_buffer_;

    // Logging subsystem
    log::LogConfig logging_config_;
    std::unique_ptr<log::LogManager> log_manager_;
    log::Logger* logger_ = nullptr;

    // Dead-letter queue
    std::unique_ptr<mailbox::DeadLetterQueue> dead_letters_;

    // Receiver dedup cache for at-least-once delivery
    std::unique_ptr<mailbox::DedupCache> dedup_cache_;

    // Tracing subsystem
    tracing::TraceConfig tracing_config_;
    std::unique_ptr<tracing::TraceManager> trace_manager_;

    // Proto type registry for protobuf message serialization
    ProtoTypeRegistry proto_registry_;

    // Pending remote spawns awaiting response
    std::unordered_map<uint64_t, std::shared_ptr<AsyncActor>> pending_spawns_;
    std::mutex pending_spawns_mutex_;
};

// -----------------------------------------------------------------------------
// Template implementations
// -----------------------------------------------------------------------------

template <typename T, typename... Args>
Actor ActorSystem::spawn(Args&&... args) {
    ActorId id(next_actor_id_.fetch_add(1));
    auto actor = std::make_shared<T>(nullptr, *this, std::forward<Args>(args)...);
    actor->set_address(ActorAddress(endpoint_, actor->type(), id, 0));
    if constexpr (requires { T::kActorTypeName; }) {
        actor->set_type_name(T::kActorTypeName);
    } else {
        actor->set_type_name("unknown");
    }

    {
        std::lock_guard<std::mutex> lock(actors_mutex_);
        actors_.emplace(id, actor);
    }

    // Create mailbox
    {
        std::lock_guard<std::mutex> lock(mailboxes_mutex_);
        mailboxes_.emplace(
            id, std::make_unique<mailbox::MPSCActorMailbox<TypedMessage>>(
                    id, scheduler_.get(), mailbox_config_for_spawn()));
    }

    // Create actor context and set it on the actor
    auto actor_ctx = std::make_unique<ActorContext>(Actor(actor), this);
    actor->set_context(actor_ctx.get());
    actor_contexts_.emplace(id, std::move(actor_ctx));

    // Set scheduler and mailbox on actor
    actor->set_scheduler(scheduler_.get());
    actor->set_mailbox(mailboxes_[id].get());

    // Wire metrics ring buffer to actor and mailbox
    if (metrics_ring_buffer_) [[unlikely]] {
        auto* mbox = mailboxes_[id].get();
        mbox->set_metrics_ring_buffer(metrics_ring_buffer_.get());
        actor->set_metrics_ring_buffer(metrics_ring_buffer_.get());
    }

    // Wire logger to actor and mailbox
    if (logger_) [[unlikely]] {
        auto* mbox = mailboxes_[id].get();
        mbox->set_logger(logger_);
        actor->set_logger(logger_);
    }

    // Register with scheduler based on dispatch policy.
    // Cooperative actors go onto the work-stealing pool. Dedicated actors
    // are registered with the scheduler but NOT placed on the cooperative
    // pool — they manage their own threads or use DedicatedThreadPool.
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

    // Activate the actor (DaemonActor starts its thread here, etc.)
    actor->on_activate();

    // Transition lifecycle to ACTIVE if actor has lifecycle management
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
