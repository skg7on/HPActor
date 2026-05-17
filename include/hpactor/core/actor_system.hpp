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

// -----------------------------------------------------------------------------
// MailboxDefaults - system-wide default mailbox configuration
// -----------------------------------------------------------------------------
struct MailboxDefaults {
    uint32_t default_capacity = 1024;
    uint64_t default_byte_capacity = 0;
    mailbox::OverflowPolicy default_policy = mailbox::OverflowPolicy::RejectNewest;
    double high_watermark = 0.80;
    double low_watermark = 0.50;
    uint32_t protected_system_messages = 32;
    mailbox::BackpressureMode backpressure_mode =
        mailbox::BackpressureMode::LocalAndRemoteSignal;
};

// -----------------------------------------------------------------------------
// Config - configuration for ActorSystem
// -----------------------------------------------------------------------------
struct Config {
    size_t scheduler_threads = 4;
    size_t max_queue_depth = 1024;
    EndPoint endpoint = LocalEndpoint;

    // Network configuration
    bool enable_network = false;
    uint16_t tcp_port = 0; // Transport TCP port (0 = don't listen)

    // Remote spawn configuration
    std::chrono::milliseconds spawn_timeout{5000};

    // TLS and pool config (used if enable_network=true)
    net::TlsConfig tls = {};
    net::PoolConfig pool = {};
    net::RegistrarConfig registrar = {};

    // HTTP subsystem (requires enable_network = true)
    bool enable_http_gateway = false;
    bool enable_http_client = false;

    // HTTP gateway configuration
    uint16_t http_port = 8080;
    std::string http_bind_host = "0.0.0.0";
    size_t http_max_connections = 1000;
    size_t http_max_request_size = 1048576;
    std::chrono::milliseconds http_reply_timeout{5000};

    // Coroutine scheduling (requires HPACTOR_SUPPORT_COROUTINES=1 at compile
    // time) When true, actors use coroutine-based execution instead of
    // behavior-based. Default: false (behavior-based scheduling).
    bool use_coroutines = false;

    // CLI configuration
    cli::CliConfig cli;

    // Service discovery backend. nullptr = auto-select based on enable_network
    // and registrar config (backward compatible).
    std::shared_ptr<net::IServiceDiscovery> service_discovery = nullptr;

    // Gossip configuration. Used when creating GossipMembership internally.
    net::GossipConfig gossip = {};

    // Mailbox defaults — applied to every actor spawned via this system
    MailboxDefaults mailbox;

    // Dead-letter queue configuration
    mailbox::DeadLetterConfig dead_letters;

    // Shutdown configuration
    DrainConfig shutdown_drain{DrainPolicy::Drain,
                               std::chrono::milliseconds{30'000}};
    uint32_t ingress_timeout_ms{5000};
    uint32_t cluster_leave_timeout_ms{10000};
    bool shutdown_force_after_timeout{true};

    // Timer backend selection
    sched::TimerBackend timer_backend = sched::TimerBackend::TimingWheel;

    // Start scheduler workers in paused state (for deterministic testing).
    // When true, workers are created but blocked until resume_workers() is
    // called.
    bool scheduler_start_paused = false;

    // Distributed tracing configuration
    tracing::TraceConfig tracing;
};

// -----------------------------------------------------------------------------
// ActorTypeDef - definition of an actor type
// -----------------------------------------------------------------------------
struct ActorTypeDef {
    std::string name;
    ActorType id;
};

// Shutdown phase enumeration
enum class ShutdownPhase : uint8_t {
    Running,
    DrainingIngress,
    DrainingActors,
    LeavingCluster,
    FlushingTelemetry,
    Stopped,
    ForcedStop,
};

struct ShutdownOptions {
    std::chrono::milliseconds ingress_timeout{5'000};
    std::chrono::milliseconds actor_drain_timeout{30'000};
    std::chrono::milliseconds cluster_leave_timeout{10'000};
    bool force_after_timeout{true};
};

// -----------------------------------------------------------------------------
// ActorSystem - the actor environment containing schedulers, registry, etc.
// -----------------------------------------------------------------------------
class ActorSystem {
  public:
    explicit ActorSystem(const Config& config);
    ~ActorSystem();

    // Non-copyable, non-movable
    ActorSystem(const ActorSystem&) = delete;
    ActorSystem& operator=(const ActorSystem&) = delete;
    ActorSystem(ActorSystem&&) = delete;
    ActorSystem& operator=(ActorSystem&&) = delete;

    // Spawn actors at system level
    template <typename T, typename... Args> Actor spawn(Args&&... args);

    // Spawn a pre-constructed actor with configuration from ActorDef.
    // Used by BootstrapEngine for TOML-based topology bootstrapping.
    Actor spawn_configured(std::shared_ptr<AbstractActor> actor,
                           const struct config::ActorDef& def);

    // Load topology from TOML file (convenience entry point)
    result<void> load_topology(const std::string& toml_path);

    // Actor registry
    void register_actor(const std::string& name, Actor actor);
    Actor resolve_actor(const std::string& name);
    void unregister_actor(const std::string& name);

    // Actor type registration
    void register_actor_type(const ActorTypeDef& def);
    ActorTypeDef get_actor_type(ActorType type) const;

    // Clock
    Clock& clock() {
        return clock_;
    }

    // System actor
    Actor system_actor() {
        return system_actor_;
    }

    // Registry access
    actor_registry& registry() {
        return registry_;
    }

    // Proto type registry
    ProtoTypeRegistry& proto_registry() {
        return proto_registry_;
    }
    const ProtoTypeRegistry& proto_registry() const {
        return proto_registry_;
    }

    // Node ID
    EndPoint endpoint() const {
        return endpoint_;
    }

    // Check if actor system is running
    bool is_running() const {
        return running_.load(std::memory_order_acquire);
    }

    // Get scheduler for direct scheduling operations
    sched::IScheduler* scheduler() {
        return scheduler_.get();
    }

    // Runtime coroutine toggle (requires HPACTOR_SUPPORT_COROUTINES=1 at
    // compile time). Default: false (behavior-based scheduling).
    bool use_coroutines() const {
        return config_.use_coroutines;
    }

    // RPC channel for remote calls
    RpcChannel& rpc_channel() {
        return *rpc_channel_;
    }

    // HTTP client for outbound HTTP requests
    net::HttpClient& http_client() {
        return *http_client_;
    }

    // Distributed tracing
    tracing::TraceManager* trace_manager() noexcept {
        return trace_manager_.get();
    }

    const tracing::TraceManager* trace_manager() const noexcept {
        return trace_manager_.get();
    }

    void apply_tracing_config(const tracing::TraceConfig& config);

    // Internal actor lookup (used by scheduler)
    std::shared_ptr<AbstractActor> get_actor(ActorId id);

    // Get metrics ring buffer (nullptr if metrics disabled)
    auto* metrics_ring_buffer() const {
        return metrics_ring_buffer_.get();
    }

    // Get CLI actor (nullptr if CLI disabled or not yet spawned)
    cli::CliActor* cli_actor() const;

    // Get actor's mailbox (used by scheduler)
    mailbox::MPSCActorMailbox<TypedMessage>* get_mailbox(ActorId id);

    // Get the number of live actors in this system
    size_t actor_count() const;

    // Enumerate all actors. Callback receives (ActorId, AbstractActor&).
    // The callback must not spawn or kill actors (lock is held).
    void for_each_actor(std::function<void(ActorId, AbstractActor&)> callback) const;

    // Deliver message to local actor
    void deliver_local(ActorId target, TypedMessage msg);

    // Deliver message to local actor with priority and deadline for scheduling
    // priority: 0-3 (0 = highest)
    // deadline_ns: absolute deadline in nanoseconds (INT64_MAX = no deadline)
    void deliver_local(ActorId target, TypedMessage msg, uint8_t priority,
                       int64_t deadline_ns);

    // Bounded admission delivery — returns an EnqueueResult describing the
    // outcome. Returns ActorNotFound when the target actor does not exist,
    // Rejected when the mailbox is at hard capacity.
    mailbox::EnqueueResult
    try_deliver_local(ActorId target, TypedMessage msg, uint8_t priority = 0,
                      int64_t deadline_ns = INT64_MAX,
                      mailbox::DeliveryOptions options = {});

    // Dead-letter queue
    bool dead_letter(mailbox::DeadLetterRecord record) noexcept;
    mailbox::DeadLetterQueueSnapshot dead_letter_snapshot() const noexcept;
    bool pop_dead_letter(mailbox::DeadLetterRecord& out) noexcept;

    // Build a MailboxConfig from system-wide defaults in Config::mailbox.
    mailbox::MailboxConfig mailbox_config_for_spawn() const;

    // Build a MailboxConfig for a specific ActorDef, falling back to system
    // defaults when ActorDef fields are zero.
    mailbox::MailboxConfig
    mailbox_config_for_actor_def(const config::ActorDef& def) const;

    // Deliver a remote message (from WireFrame) to the target actor's mailbox.
    // Bridges the transport layer to the unified deliver_local() sink.
    void deliver_remote(const net::WireFrame& frame);

    // Called by IServiceDiscovery when a remote node becomes unreachable.
    // Finds all actors linked to the dead endpoint and delivers DownMsg.
    void on_node_dead(EndPoint dead_ep);

    // Emit a backpressure signal to the sender actor, delivered through
    // the sender's ActorContext::handle_backpressure() handler.
    void signal_backpressure(const mailbox::BackpressureSignal& signal);

    // Enqueue an I/O completion to be delivered to an actor
    // Called by EventLoop when async operations complete
    void enqueue_completion(net::OpCompletion completion);

    // Network access
    net::Transport* transport() {
        return transport_.get();
    }

    // Return the transport for sending to a remote endpoint.
    // Currently returns the single transport_ for all remote endpoints, since
    // TcpTransport handles per-endpoint routing internally via its pools_ map.
    // The endpoint parameter is reserved for future multi-transport scenarios.
    // Returns nullptr if networking is not enabled.
    net::Transport* get_transport_for(const EndPoint& endpoint);

    net::UdpRegistrar* registrar() {
        return registrar_.get();
    }

    // Remote actor spawning (main/non-actor context only)
    result<ActorRef>
    spawn_remote(const std::string& node_name, const std::string& actor_type,
                 const StreamBuffer& args);

    AsyncActor
    spawn_remote_async(const std::string& node_name,
                       const std::string& actor_type, const StreamBuffer& args);

    // Actor type registry for remote spawning
    ActorTypeRegistry& actor_type_registry() {
        return *actor_type_registry_;
    }
    const ActorTypeRegistry& actor_type_registry() const {
        return *actor_type_registry_;
    }

    // ── Shutdown ───────────────────────────────────────────────────────────

    // Node shutdown — drives the full phase machine.
    // Overload with no arguments uses default ShutdownOptions{}.
    result<void> shutdown();
    result<void> shutdown(const ShutdownOptions& opts);

    // Current shutdown phase
    ShutdownPhase shutdown_phase() const noexcept;

    // Health/readiness gating
    bool is_ready() const noexcept;
    bool is_draining() const noexcept;

    // Per-actor drain config override (for admin/CLI use)
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
    std::atomic<ActorId::counter_type> next_actor_id_{1};

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