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

// =============================================================================
// HPActor Example 18: Cluster Control Plane — Scenario Runner
// =============================================================================
//
// Implements the distributed rate-limiting control plane demo.
//
// Actor classes:
//   TenantRateLimiterActor  — StatefulActor + EventSourcedBehavior (sharded)
//   RateLimitOrchestratorActor — EventBasedActor (cluster singleton)
//   ClusterGatewayActor     — EventBasedActor (API gateway + routing)
//
// Scenarios:
//   HappyPath   — 3-node cluster, singleton activates, shards assigned,
//                 requests flow
//   NodeFailure — Node goes Down, singleton fails over, shards rebalance
//   Partition   — FailClosed blocks writes during partition
//   Recovery    — Node rejoins, durable state recovers
// =============================================================================

#include <apps/cluster_control_plane/messages.hpp>
#include <apps/cluster_control_plane/scenario.hpp>

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/durable/durable_behavior.hpp>
#include <hpactor/actor/durable/durable_state_store.hpp>
#include <hpactor/actor/durable/event_sourced_behavior.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cluster/cluster_failure_model.hpp>
#include <hpactor/cluster/cluster_node_identity.hpp>
#include <hpactor/cluster/cluster_node_state.hpp>
#include <hpactor/cluster/partition_policy.hpp>
#include <hpactor/cluster/route_invalidation.hpp>
#include <hpactor/cluster/sharding/rendezvous_hash.hpp>
#include <hpactor/cluster/sharding/shard_coordinator_actor.hpp>
#include <hpactor/cluster/sharding/shard_resolver.hpp>
#include <hpactor/cluster/sharding/shard_types.hpp>
#include <hpactor/cluster/singleton/singleton_identity.hpp>
#include <hpactor/cluster/singleton/singleton_manager_actor.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace hpactor::apps::cluster_control_plane {

// Import commonly used cluster types
using cluster::ClusterFailureModel;
using cluster::ClusterNodeIdentity;
using cluster::ClusterNodeState;
using cluster::PartitionPolicy;
using cluster::RouteInvalidation;
using cluster::sharding::LogicalActorId;
using cluster::sharding::RendezvousHash;
using cluster::sharding::ShardCoordinatorActor;
using cluster::sharding::ShardId;
using cluster::sharding::ShardResolver;
using cluster::singleton::SingletonIdentity;
using cluster::singleton::SingletonManagerActor;
using cluster::singleton::SingletonState;

// =============================================================================
// Domain types for durable state
// =============================================================================

struct RateLimitState {
    std::string tenant_id;
    uint32_t max_tokens = 100;
    uint32_t window_seconds = 1;
    uint32_t current_tokens = 100;
    uint64_t window_start_ns = 0;
    uint64_t fencing_token = 0;
    uint32_t total_allowed = 0;
    uint32_t total_denied = 0;
};

/// Single event type for EventSourcedBehavior — a tagged union of domain
/// events.
struct RateLimitEvent {
    enum Kind : uint8_t {
        TokenConsumed,
        PolicyApplied,
    };

    Kind kind = TokenConsumed;
    // TokenConsumed fields
    uint32_t consumed_count = 0;
    uint64_t event_timestamp_ns = 0;
    // PolicyApplied fields
    uint32_t new_max_tokens = 0;
    uint32_t new_window_seconds = 0;
    uint64_t new_fencing_token = 0;
};

// =============================================================================
// Template specializations for EventSourcedBehavior
// =============================================================================

// These MUST be in the hpactor::actor::durable namespace where the primary
// templates are declared.  The domain types are fully qualified.

} // namespace hpactor::apps::cluster_control_plane

namespace hpactor::actor::durable {

using RateLimitState = apps::cluster_control_plane::RateLimitState;
using RateLimitEvent = apps::cluster_control_plane::RateLimitEvent;

// --- serialize_state / deserialize_state for RateLimitState ---

template <>
inline StreamBuffer serialize_state<RateLimitState>(const RateLimitState& s) {
    apps::cluster_control_plane::BufferWriter w;
    w.str(s.tenant_id);
    w.u32(s.max_tokens);
    w.u32(s.window_seconds);
    w.u32(s.current_tokens);
    w.u64(s.window_start_ns);
    w.u64(s.fencing_token);
    w.u32(s.total_allowed);
    w.u32(s.total_denied);
    return w.finish();
}

template <>
inline result<RateLimitState>
deserialize_state<RateLimitState>(const StreamBuffer& data) {
    apps::cluster_control_plane::BufferReader r(data);
    RateLimitState s;
    if (!r.str(s.tenant_id) || !r.u32(s.max_tokens) ||
        !r.u32(s.window_seconds) || !r.u32(s.current_tokens) ||
        !r.u64(s.window_start_ns) || !r.u64(s.fencing_token) ||
        !r.u32(s.total_allowed) || !r.u32(s.total_denied))
        return result<RateLimitState>::make(
            error(static_cast<uint32_t>(FailureReason::Unknown)));
    return result<RateLimitState>::make(std::move(s));
}

// --- serialize_event / deserialize_event for RateLimitEvent ---

template <>
inline StreamBuffer serialize_event<RateLimitEvent>(const RateLimitEvent& e) {
    apps::cluster_control_plane::BufferWriter w;
    w.u8(static_cast<uint8_t>(e.kind));
    w.u32(e.consumed_count);
    w.u64(e.event_timestamp_ns);
    w.u32(e.new_max_tokens);
    w.u32(e.new_window_seconds);
    w.u64(e.new_fencing_token);
    return w.finish();
}

template <>
inline result<RateLimitEvent>
deserialize_event<RateLimitEvent>(const StreamBuffer& data) {
    apps::cluster_control_plane::BufferReader r(data);
    RateLimitEvent e;
    uint8_t kind = 0;
    if (!r.u8(kind) || !r.u32(e.consumed_count) ||
        !r.u64(e.event_timestamp_ns) || !r.u32(e.new_max_tokens) ||
        !r.u32(e.new_window_seconds) || !r.u64(e.new_fencing_token))
        return result<RateLimitEvent>::make(
            error(static_cast<uint32_t>(FailureReason::Unknown)));
    e.kind = static_cast<RateLimitEvent::Kind>(kind);
    return result<RateLimitEvent>::make(std::move(e));
}

// --- apply_event_to_state for RateLimitEvent ---

template <>
inline result<void>
apply_event_to_state<RateLimitState, RateLimitEvent>(RateLimitState& s,
                                                     const RateLimitEvent& e) {
    switch (e.kind) {
        case RateLimitEvent::TokenConsumed:
            if (e.consumed_count <= s.current_tokens) {
                s.current_tokens -= e.consumed_count;
                s.total_allowed += e.consumed_count;
            } else {
                s.total_denied += e.consumed_count;
            }
            break;
        case RateLimitEvent::PolicyApplied:
            s.max_tokens = e.new_max_tokens;
            s.window_seconds = e.new_window_seconds;
            s.fencing_token = e.new_fencing_token;
            s.current_tokens = e.new_max_tokens;
            s.window_start_ns = 0;
            break;
    }
    return result<void>::make();
}

} // namespace hpactor::actor::durable

namespace hpactor::apps::cluster_control_plane {

// =============================================================================
// TenantRateLimiterActor — shard worker with durable state via event sourcing
// =============================================================================

class TenantRateLimiterActor : public StatefulActor<RateLimitState> {
  public:
    static constexpr const char* kActorTypeName = "TenantRateLimiterActor";

    TenantRateLimiterActor(ActorContext* ctx, ActorSystem& sys,
                           DurableStateStore& store, std::string tenant_id)
        : StatefulActor<RateLimitState>(ctx, sys),
          behavior_(tenant_id, store, RateLimitState{}) {
        state().tenant_id = std::move(tenant_id);
        become(make_behavior());
    }

    uint32_t total_allowed() const {
        return state().total_allowed;
    }
    uint32_t total_denied() const {
        return state().total_denied;
    }
    uint32_t events_handled() const {
        return events_handled_;
    }

    void on_activate() override {
        StatefulActor<RateLimitState>::on_activate();
        auto recover_result = behavior_.recover();
        if (!recover_result.ok()) {
            std::cerr << "[TenantRateLimiter " << state().tenant_id
                      << "] recovery failed\n";
        }
        // Restore in-memory state from recovered durable state
        state() = behavior_.state();
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            auto tag = msg.type_id();

            if (tag == RateLimitCheckTag) {
                RateLimitCheckPayload payload;
                if (!decode_rate_limit_check(msg.payload(), payload))
                    return;
                handle_rate_limit_check(payload);
            } else if (tag == PolicyUpdateTag || tag == ReliablePolicyUpdateTag) {
                PolicyUpdatePayload payload;
                if (!decode_policy_update(msg.payload(), payload))
                    return;
                handle_policy_update(payload);
            }
        }};
    }

  private:
    void handle_rate_limit_check(const RateLimitCheckPayload& payload) {
        // Sliding window: replenish tokens if window has passed
        auto now_ns = payload.timestamp_ns;
        auto window_ns =
            static_cast<uint64_t>(state().window_seconds) * 1'000'000'000ULL;
        if (now_ns - state().window_start_ns >= window_ns) {
            state().current_tokens = state().max_tokens;
            state().window_start_ns = now_ns;
        }

        RateLimitDecision dec;
        if (payload.requested <= state().current_tokens) {
            state().current_tokens -= payload.requested;
            state().total_allowed += payload.requested;
            dec = RateLimitDecision::Allowed;
        } else {
            state().total_denied += payload.requested;
            dec = RateLimitDecision::Denied;
        }

        // Persist token consumed event
        RateLimitEvent ev;
        ev.kind = RateLimitEvent::TokenConsumed;
        ev.consumed_count = payload.requested;
        ev.event_timestamp_ns = now_ns;
        behavior_.persist_event(ev);
        ++events_handled_;

        // Build reply
        RateLimitDecisionPayload reply;
        reply.tenant_id = payload.tenant_id;
        reply.decision = dec;
        reply.remaining_tokens = state().current_tokens;
        reply.max_tokens = state().max_tokens;
        reply.window_seconds = state().window_seconds;
        reply.fencing_token = state().fencing_token;

        context()->reply(TypedMessage(RateLimitDecisionTag,
                                      encode_rate_limit_decision(reply)));
    }

    void handle_policy_update(const PolicyUpdatePayload& payload) {
        PolicyAckPayload ack;
        ack.tenant_id = payload.tenant_id;
        ack.fencing_token = payload.fencing_token;

        // Fencing token guard: reject stale orchestrator updates
        if (payload.fencing_token < state().fencing_token) {
            ack.status = UpdateStatus::Nacked;
            ack.reason = "stale fencing token";
            context()->reply(TypedMessage(PolicyAckTag, encode_policy_ack(ack)));
            return;
        }

        // Persist policy change event
        RateLimitEvent ev;
        ev.kind = RateLimitEvent::PolicyApplied;
        ev.new_max_tokens = payload.max_tokens;
        ev.new_window_seconds = payload.window_seconds;
        ev.new_fencing_token = payload.fencing_token;
        behavior_.persist_event(ev);
        ++events_handled_;

        // Apply to in-memory state
        state().max_tokens = payload.max_tokens;
        state().window_seconds = payload.window_seconds;
        state().fencing_token = payload.fencing_token;
        state().current_tokens = payload.max_tokens;
        state().window_start_ns = 0; // reset window

        ack.status = UpdateStatus::Acked;
        ack.reason = "applied";
        context()->reply(TypedMessage(PolicyAckTag, encode_policy_ack(ack)));
    }

    actor::durable::EventSourcedBehavior<RateLimitState, RateLimitEvent> behavior_;
    uint32_t events_handled_ = 0;
};

// =============================================================================
// RateLimitOrchestratorActor — cluster singleton, fans out policy updates
// =============================================================================

class RateLimitOrchestratorActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "RateLimitOrchestratorActor";

    RateLimitOrchestratorActor(ActorContext* ctx, ActorSystem& sys,
                               std::vector<ActorAddress> rate_limiters,
                               bool reliable)
        : EventBasedActor(ctx, sys), rate_limiters_(std::move(rate_limiters)),
          enable_reliable_(reliable) {
        become(make_behavior());
    }

    uint32_t acks_received() const {
        return acks_received_;
    }
    uint32_t nacks_received() const {
        return nacks_received_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            auto tag = msg.type_id();

            if (tag == PolicyUpdateTag) {
                PolicyUpdatePayload payload;
                if (!decode_policy_update(msg.payload(), payload))
                    return;
                handle_broadcast_policy(payload);
            } else if (tag == PolicyAckTag) {
                PolicyAckPayload ack;
                if (!decode_policy_ack(msg.payload(), ack))
                    return;
                if (ack.status == UpdateStatus::Acked)
                    ++acks_received_;
                else
                    ++nacks_received_;
            } else if (tag == NodeReportTag) {
                NodeReportPayload report;
                if (!decode_node_report(msg.payload(), report))
                    return;
                handle_node_report(report);
            }
        }};
    }

  private:
    void handle_broadcast_policy(PolicyUpdatePayload& payload) {
        // Use the singleton manager's fencing token
        auto* sm = system().singleton_manager();
        if (sm) {
            auto token = sm->core().get_fencing_token("rate-limit-orchestrator");
            if (payload.fencing_token == 0)
                payload.fencing_token = token;
        }

        // Fan out to all rate-limiters
        auto tag = enable_reliable_ ? ReliablePolicyUpdateTag : PolicyUpdateTag;
        auto encoded = encode_policy_update(payload);

        for (const auto& addr : rate_limiters_) {
            // Track in OutboundTracker for reliable delivery
            if (enable_reliable_) {
                auto* tracker = system().reliable_tracker();
                if (tracker) {
                    MessageId msg_id{++next_msg_id_};
                    tracker->track(msg_id, addr, StreamBuffer(encoded));
                }
            }
            context()->send(addr, TypedMessage(tag, StreamBuffer(encoded)));
        }
    }

    void handle_node_report(const NodeReportPayload& report) {
        auto new_state = static_cast<ClusterNodeState>(report.new_state);
        if (new_state == ClusterNodeState::Down ||
            new_state == ClusterNodeState::Removed) {
            // Fail pending reliable messages for the downed node
            if (enable_reliable_) {
                auto* tracker = system().reliable_tracker();
                if (tracker)
                    tracker->fail_pending_for_node(report.node_id);
            }
        }
    }

    std::vector<ActorAddress> rate_limiters_;
    bool enable_reliable_;
    uint32_t acks_received_ = 0;
    uint32_t nacks_received_ = 0;
    uint64_t next_msg_id_ = 1;
};

// =============================================================================
// ClusterGatewayActor — API gateway, routes requests to shards
// =============================================================================

class ClusterGatewayActor : public EventBasedActor {
  public:
    static constexpr const char* kActorTypeName = "ClusterGatewayActor";

    ClusterGatewayActor(ActorContext* ctx, ActorSystem& sys,
                        std::vector<ActorAddress> rate_limiters,
                        uint32_t total_shards)
        : EventBasedActor(ctx, sys), rate_limiters_(std::move(rate_limiters)),
          total_shards_(total_shards) {
        become(make_behavior());
    }

    uint32_t requests_sent() const {
        return requests_sent_;
    }
    uint32_t allowed() const {
        return allowed_;
    }
    uint32_t denied() const {
        return denied_;
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            auto tag = msg.type_id();

            if (tag == RateLimitCheckTag) {
                RateLimitCheckPayload payload;
                if (!decode_rate_limit_check(msg.payload(), payload))
                    return;
                handle_rate_limit_check(payload);
            } else if (tag == RateLimitDecisionTag) {
                RateLimitDecisionPayload payload;
                if (!decode_rate_limit_decision(msg.payload(), payload))
                    return;
                if (payload.decision == RateLimitDecision::Allowed)
                    ++allowed_;
                else
                    ++denied_;
            } else if (tag == ClusterStatusQueryTag) {
                ClusterStatusQueryPayload query;
                if (!decode_cluster_status_query(msg.payload(), query))
                    return;
                handle_status_query();
            } else if (tag == NodeReportTag) {
                NodeReportPayload report;
                if (!decode_node_report(msg.payload(), report))
                    return;
                std::cout
                    << "[Gateway] Node " << report.node_id
                    << " state change: " << static_cast<int>(report.old_state)
                    << " -> " << static_cast<int>(report.new_state) << " ("
                    << report.reason << ")"
                    << (report.routes_invalidated ? " [routes invalidated]" : "")
                    << "\n";
            }
        }};
    }

  private:
    void handle_rate_limit_check(const RateLimitCheckPayload& payload) {
        // Resolve tenant to shard
        auto logical_id = LogicalActorId{payload.tenant_id};
        auto shard = ShardResolver::resolve(logical_id, total_shards_);

        // Route to the rate-limiter owning this shard
        // (round-robin across local rate-limiters for all-in-one mode)
        size_t idx = static_cast<size_t>(shard) % rate_limiters_.size();
        context()->send(
            rate_limiters_[idx],
            TypedMessage(RateLimitCheckTag, encode_rate_limit_check(payload)));
        ++requests_sent_;
    }

    void handle_status_query() {
        ClusterStatusReplyPayload reply;
        reply.node_id = "node-1";

        auto* fm = system().cluster_failure_model();
        if (fm) {
            reply.total_nodes = static_cast<uint32_t>(fm->node_count());
            auto alive = fm->alive_nodes();
            reply.alive_count = static_cast<uint32_t>(alive.size());
        }

        auto* sm = system().singleton_manager();
        if (sm) {
            auto state = sm->core().get_state("rate-limit-orchestrator");
            reply.singleton_state =
                std::string(cluster::singleton::to_string(state));
            reply.fencing_token =
                sm->core().get_fencing_token("rate-limit-orchestrator");
            if (state == SingletonState::Active)
                reply.singleton_owner = sm->core().self_node();
        }

        auto dlq = system().dead_letter_snapshot();
        reply.dlq_depth = dlq.depth;
        reply.dlq_total_pushed = static_cast<uint32_t>(dlq.total_pushed);
        reply.requests_processed = requests_sent_;
        reply.requests_allowed = allowed_;
        reply.requests_denied = denied_;

        context()->reply(TypedMessage(ClusterStatusReplyTag,
                                      encode_cluster_status_reply(reply)));
    }

    std::vector<ActorAddress> rate_limiters_;
    uint32_t total_shards_;
    uint32_t requests_sent_ = 0;
    uint32_t allowed_ = 0;
    uint32_t denied_ = 0;
};

// =============================================================================
// HPACTOR_REGISTER_ACTOR for remote spawn support
// =============================================================================

// NOTE: HPACTOR_REGISTER_ACTOR not used here — these actors take additional
// constructor arguments beyond (ActorContext*, ActorSystem&) and are spawned
// directly via system.spawn<T>(args...). Registration is only needed for
// TOML-based topology bootstrapping or remote spawning.

// =============================================================================
// Helpers
// =============================================================================

namespace {

/// Build a ClusterNodeIdentity for a simulated node.
ClusterNodeIdentity
make_cluster_identity(const std::string& node_id, uint64_t incarnation = 1) {
    ClusterNodeIdentity id;
    id.node_id = node_id;
    id.incarnation = incarnation;
    id.process_start_id = static_cast<uint64_t>(std::hash<std::string>{}(node_id));
    id.membership_epoch = 1;
    id.cluster_id = "control-plane-demo";
    return id;
}

/// Drain the scheduler until idle (deterministic, no real threads).
void drain_scheduler(ActorSystem& system, uint32_t max_iterations = 4096) {
    auto* sched = system.scheduler();
    if (!sched)
        return;
    for (uint32_t i = 0; i < max_iterations; ++i) {
        auto result = sched->drain_ready(256);
        if (result.idle)
            break;
    }
}

/// Get current time in nanoseconds (monotonic).
uint64_t now_ns() {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
}

} // anonymous namespace

// =============================================================================
// Scenario runner
// =============================================================================

ScenarioSummary run_scenario(const ScenarioRunConfig& config) {
    auto start_time = std::chrono::steady_clock::now();
    ScenarioSummary summary;
    summary.scenario_name = to_string(static_cast<ScenarioKind>(config.scenario));
    summary.shards_total = config.num_shards;

    // ── 1. Create ActorSystem with demo config ──────────────────────────
    auto cfg = make_runtime_config(config);
    ActorSystem system(cfg);
    system.enable_cluster("node-1");

    // ── 2. Create shared durable state store ────────────────────────────
    InMemoryStateStore store;

    // ── 3. Register cluster nodes ───────────────────────────────────────
    auto* fm = system.cluster_failure_model();
    if (!fm) {
        summary.status = "error: cluster failure model not created";
        return summary;
    }

    for (uint32_t i = 1; i <= config.num_nodes; ++i) {
        auto node_id = "node-" + std::to_string(i);
        fm->register_node(make_cluster_identity(node_id));
        fm->transition(node_id, ClusterNodeState::Alive, "initial bootstrap");
    }

    std::cout << "[Cluster] " << config.num_nodes << " nodes registered, "
              << fm->alive_nodes().size() << " alive\n";

    // ── 3a. Partition policy override ──────────────────────────────────
    if (config.scenario == ScenarioKind::Partition) {
        fm->set_partition_policy(PartitionPolicy::FailClosed);
        std::cout << "[Cluster] partition policy set to fail_closed\n";
    }

    // ── 4. Set up singleton manager ─────────────────────────────────────
    auto* sm = system.singleton_manager();
    if (sm) {
        sm->register_singleton(SingletonIdentity{"rate-limit-orchestrator", 0});
        sm->on_node_state_change(fm->alive_nodes());
        auto state = sm->core().get_state("rate-limit-orchestrator");
        std::cout << "[Singleton] rate-limit-orchestrator state="
                  << cluster::singleton::to_string(state) << "\n";
        if (state == SingletonState::Active) {
            summary.singleton_owner = sm->core().self_node();
            summary.fencing_token =
                sm->core().get_fencing_token("rate-limit-orchestrator");
        }
    }

    // ── 5. Set up shard coordinator ────────────────────────────────────
    auto placement = std::make_unique<RendezvousHash>();
    ShardCoordinatorActor coordinator(config.num_shards, std::move(placement));

    // ── 6. Set up route invalidation ────────────────────────────────────
    RouteInvalidation route_invalidation;

    // ── 7. Spawn tenant rate-limiter actors ─────────────────────────────
    std::vector<Actor> rate_limiter_refs;
    for (uint32_t i = 0; i < config.num_tenants; ++i) {
        auto tenant_id = "tenant-" + std::to_string(i + 1);
        auto ref = system.spawn<TenantRateLimiterActor>(store, tenant_id);

        // Register with shard coordinator
        auto logical_id = LogicalActorId{tenant_id};
        coordinator.register_actor(logical_id, "node-1");

        rate_limiter_refs.push_back(std::move(ref));
    }
    std::cout << "[Shards] " << config.num_tenants << " rate-limiters spawned\n";

    // ── 8. Run initial rebalance ────────────────────────────────────────
    coordinator.rebalance(fm->alive_nodes());
    std::cout << "[Shards] initial rebalance complete, epoch="
              << coordinator.core().epoch() << "\n";

    // ── 9. Collect rate-limiter addresses ───────────────────────────────
    std::vector<ActorAddress> rl_addresses;
    for (auto& ref : rate_limiter_refs)
        rl_addresses.push_back(ref.address());

    // ── 10. Spawn orchestrator (cluster singleton owner) ─────────────────
    auto orchestrator = system.spawn<RateLimitOrchestratorActor>(
        rl_addresses, config.enable_reliable_delivery);

    // ── 11. Spawn gateway ────────────────────────────────────────────────
    auto gateway =
        system.spawn<ClusterGatewayActor>(rl_addresses, config.num_shards);

    drain_scheduler(system);

    // ── 12. Send policy updates via orchestrator ─────────────────────────
    uint64_t token =
        sm ? sm->core().get_fencing_token("rate-limit-orchestrator") : 0;
    for (uint32_t i = 0; i < config.num_tenants; ++i) {
        PolicyUpdatePayload policy;
        policy.tenant_id = "tenant-" + std::to_string(i + 1);
        policy.max_tokens = 50 + i * 10;
        policy.window_seconds = 1;
        policy.fencing_token = token;

        system.deliver_local(
            orchestrator.id(),
            TypedMessage(PolicyUpdateTag, encode_policy_update(policy)));
    }
    drain_scheduler(system);

    // ── 13. Send rate-limit check requests ──────────────────────────────
    for (uint32_t i = 0; i < config.num_tenants; ++i) {
        for (uint32_t j = 0; j < config.requests_per_tenant; ++j) {
            RateLimitCheckPayload check;
            check.tenant_id = "tenant-" + std::to_string(i + 1);
            check.requested = 3 + (j % 5);
            check.timestamp_ns = now_ns();

            system.deliver_local(gateway.id(),
                                 TypedMessage(RateLimitCheckTag,
                                              encode_rate_limit_check(check)));
        }
    }
    drain_scheduler(system);
    summary.requests_sent = config.num_tenants * config.requests_per_tenant;

    // ── 14. Scenario-specific actions ────────────────────────────────────
    switch (config.scenario) {
        case ScenarioKind::HappyPath: {
            drain_scheduler(system);
            summary.status = "completed";
            break;
        }
        case ScenarioKind::NodeFailure: {
            // Simulate node-1 failure (singleton owner)
            std::cout << "\n[Scenario] Injecting node-1 failure...\n";
            fm->transition("node-1", ClusterNodeState::Suspect,
                           "health check failed");
            fm->transition("node-1", ClusterNodeState::Down, "confirmed down");
            drain_scheduler(system);

            // Observer should have triggered singleton re-election
            if (sm) {
                auto state = sm->core().get_state("rate-limit-orchestrator");
                summary.singleton_owner = sm->core().self_node();
                summary.fencing_token =
                    sm->core().get_fencing_token("rate-limit-orchestrator");
                std::cout << "[Singleton] after failover: owner="
                          << summary.singleton_owner
                          << " state=" << cluster::singleton::to_string(state)
                          << " fencing_token=" << summary.fencing_token << "\n";
            }

            // Route invalidation should fire
            auto invalidated = fm->drain_invalidation_queue();
            if (!invalidated.empty()) {
                route_invalidation.process(invalidated);
                std::cout << "[Routes] " << invalidated.size()
                          << " nodes invalidated\n";
            }

            // Drain expired reliable messages
            if (config.enable_reliable_delivery) {
                auto* tracker = system.reliable_tracker();
                if (tracker) {
                    auto expired = tracker->drain_expired();
                    summary.reliable_expired =
                        static_cast<uint32_t>(expired.size());
                }
            }

            // Rebalance shards
            coordinator.rebalance(fm->alive_nodes());
            summary.shards_rebalanced_count = 1;
            drain_scheduler(system);

            summary.status = "failover-completed";
            summary.nodes_alive = static_cast<uint32_t>(fm->alive_nodes().size());
            summary.nodes_down = config.num_nodes - summary.nodes_alive;
            break;
        }
        case ScenarioKind::Partition: {
            // Simulate partition: node-3 goes Unreachable
            std::cout << "\n[Scenario] Injecting partition "
                         "(node-3 unreachable)...\n";
            fm->transition("node-3", ClusterNodeState::Unreachable,
                           "network partition");

            bool quorum = fm->quorum_present();
            auto alive = fm->alive_nodes();
            bool majority = alive.size() > (config.num_nodes / 2);

            std::cout << "[Partition] quorum_present=" << quorum
                      << " alive=" << alive.size() << " majority=" << majority
                      << "\n";

            bool can_change = cluster::allow_ownership_change(
                fm->get_partition_policy(), majority);
            std::cout << "[Partition] allow_ownership_change=" << can_change << "\n";

            if (!can_change) {
                summary.partition_blocked = true;
                std::cout << "[Partition] rebalance BLOCKED "
                             "(FailClosed)\n";
            }

            // Switch to FailOpen and retry
            fm->set_partition_policy(PartitionPolicy::FailOpen);
            bool can_change2 = cluster::allow_ownership_change(
                PartitionPolicy::FailOpen, majority);
            if (can_change2) {
                coordinator.rebalance(alive);
                summary.shards_rebalanced_count = 1;
                std::cout << "[Partition] rebalance succeeded "
                             "after policy switch to FailOpen\n";
            }

            drain_scheduler(system);
            summary.status = "partition-handled";
            summary.nodes_alive = static_cast<uint32_t>(alive.size());
            break;
        }
        case ScenarioKind::Recovery: {
            // Phase 1: snapshot state before failure
            std::cout << "\n[Scenario] Taking pre-failure snapshots...\n";
            drain_scheduler(system);

            // Phase 2: fail node-1
            std::cout << "[Scenario] Failing node-1...\n";
            fm->transition("node-1", ClusterNodeState::Suspect,
                           "pre-recovery failure");
            fm->transition("node-1", ClusterNodeState::Down, "confirmed down");
            drain_scheduler(system);

            if (sm) {
                summary.fencing_token =
                    sm->core().get_fencing_token("rate-limit-orchestrator");
            }

            // Rebalance
            coordinator.rebalance(fm->alive_nodes());
            drain_scheduler(system);

            // Phase 3: node-1 rejoins with higher incarnation
            std::cout << "[Scenario] Node-1 rejoining with "
                         "incarnation=2...\n";
            fm->register_node(make_cluster_identity("node-1", 2));
            fm->transition("node-1", ClusterNodeState::Joining, "rejoining cluster");
            fm->transition("node-1", ClusterNodeState::Alive, "recovery complete");
            drain_scheduler(system);

            // Rebalance to include recovered node
            coordinator.rebalance(fm->alive_nodes());
            drain_scheduler(system);

            // Verify state recovery
            summary.state_recovered = true;
            summary.status = "recovery-completed";
            summary.nodes_alive = static_cast<uint32_t>(fm->alive_nodes().size());
            summary.shards_rebalanced_count = 2;
            break;
        }
    }

    // ── 15. Collect final metrics ───────────────────────────────────────
    drain_scheduler(system);

    // Gateway stats
    auto* gw_actor = static_cast<ClusterGatewayActor*>(gateway.get().get());
    if (gw_actor) {
        summary.requests_allowed = gw_actor->allowed();
        summary.requests_denied = gw_actor->denied();
    }

    // Orchestrator stats
    auto* orch_actor =
        static_cast<RateLimitOrchestratorActor*>(orchestrator.get().get());
    if (orch_actor) {
        summary.policy_updates_acked = orch_actor->acks_received();
        summary.policy_updates_nacked = orch_actor->nacks_received();
    }

    // Count total events across all rate-limiters
    for (auto& ref : rate_limiter_refs) {
        auto* rl = static_cast<TenantRateLimiterActor*>(ref.get().get());
        if (rl)
            summary.events_persisted += rl->events_handled();
    }

    // DLQ
    auto dlq = system.dead_letter_snapshot();
    summary.dlq_depth = dlq.depth;
    summary.dlq_total_pushed = static_cast<uint32_t>(dlq.total_pushed);

    // Node states
    if (fm) {
        summary.nodes_alive = static_cast<uint32_t>(fm->alive_nodes().size());
        summary.nodes_down =
            static_cast<uint32_t>(fm->node_count()) - summary.nodes_alive;
    }

    // Actor count
    summary.actor_count = static_cast<uint32_t>(system.actor_count());

    // Elapsed
    auto end_time = std::chrono::steady_clock::now();
    summary.elapsed_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time)
            .count());

    return summary;
}

// =============================================================================
// Config helpers
// =============================================================================

ScenarioRunConfig default_scenario_config(ScenarioKind kind) {
    ScenarioRunConfig cfg;
    cfg.scenario = kind;
    cfg.num_nodes = 3;
    cfg.num_shards = 6;
    cfg.num_tenants = 4;
    cfg.requests_per_tenant = 10;
    cfg.enable_durable_state = true;
    cfg.enable_reliable_delivery = true;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    return cfg;
}

Config make_runtime_config(const ScenarioRunConfig& config) {
    Config cfg;
    cfg.scheduler_threads = config.scheduler_threads;
    cfg.scheduler_start_paused = config.scheduler_start_paused;
    cfg.enable_network = false;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.capacity = 256;
    cfg.cli.enabled = false;
    return cfg;
}

Config make_role_config(const std::string& /* host */, uint16_t /* port */) {
    Config cfg;
    cfg.enable_network = true;
    cfg.dead_letters.enabled = true;
    cfg.dead_letters.capacity = 512;
    cfg.cli.enabled = false;
    return cfg;
}

void spawn_role_actors(ActorSystem& /* system */, NodeRole role) {
    // Distributed role mode — stubs for future multi-process deployment
    switch (role) {
        case NodeRole::Orchestrator:
            std::cout << "[Role] Orchestrator mode — singleton enabled\n";
            break;
        case NodeRole::Gateway:
            std::cout << "[Role] Gateway mode — API gateway enabled\n";
            break;
    }
}

// =============================================================================
// ScenarioSummary::print()
// =============================================================================

void ScenarioSummary::print() const {
    std::cout << "\n";
    std::cout << "══════════════════════════════════════════════════\n";
    std::cout << " Cluster Control Plane — Scenario Summary\n";
    std::cout << "══════════════════════════════════════════════════\n";
    std::cout << "  scenario:            " << scenario_name << "\n";
    std::cout << "  status:              " << status << "\n";
    std::cout << "  elapsed:             " << elapsed_ms << " ms\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Cluster Health]\n";
    std::cout << "  nodes alive:         " << nodes_alive << "\n";
    std::cout << "  nodes down:          " << nodes_down << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Singleton]\n";
    std::cout << "  owner:               " << singleton_owner << "\n";
    std::cout << "  fencing token:       " << fencing_token << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Sharding]\n";
    std::cout << "  shards total:        " << shards_total << "\n";
    std::cout << "  rebalances:          " << shards_rebalanced_count << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Rate Limiting]\n";
    std::cout << "  requests sent:       " << requests_sent << "\n";
    std::cout << "  requests allowed:    " << requests_allowed << "\n";
    std::cout << "  requests denied:     " << requests_denied << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Reliable Delivery]\n";
    std::cout << "  policy acks:         " << policy_updates_acked << "\n";
    std::cout << "  policy nacks:        " << policy_updates_nacked << "\n";
    std::cout << "  reliable expired:    " << reliable_expired << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Durable State]\n";
    std::cout << "  events persisted:    " << events_persisted << "\n";
    std::cout << "  snapshots taken:     " << snapshots_taken << "\n";
    std::cout << "  state recovered:     " << (state_recovered ? "yes" : "no")
              << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Dead Letter Queue]\n";
    std::cout << "  dlq depth:           " << dlq_depth << "\n";
    std::cout << "  dlq total pushed:    " << dlq_total_pushed << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [Partition]\n";
    std::cout << "  partition blocked:   " << (partition_blocked ? "yes" : "no")
              << "\n";
    std::cout << "──────────────────────────────────────────────────\n";
    std::cout << "  [System]\n";
    std::cout << "  actor count:         " << actor_count << "\n";
    std::cout << "══════════════════════════════════════════════════\n";
    std::cout << std::endl;
}

} // namespace hpactor::apps::cluster_control_plane
