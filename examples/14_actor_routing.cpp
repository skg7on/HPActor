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
// HPActor Example 14: Actor Routing — PoolRouter, GroupRouter, Broadcasting
// =============================================================================
//
// Demonstrates the complete actor routing subsystem:
//
//   Demo 1 — PoolRouter + RoundRobinLogic: uniform work distribution across a
//            pool of worker actors.
//   Demo 2 — PoolRouter + ConsistentHashingLogic: sticky routing so messages
//            with the same key always land on the same routee.
//   Demo 3 — Broadcast: sending a message to every routee simultaneously.
//   Demo 4 — Pool resize: scaling the pool up and down at runtime.
//   Demo 5 — Routee failure + supervision restart: a failing routee is
//            automatically replaced by the pool supervisor.
//   Demo 6 — Runtime routing logic swap: changing from RoundRobin to Random
//            mid-flight.
//   Demo 7 — GroupRouter: routing to externally-registered actors by service
//            key, with dynamic routee add/remove.
//
// Concepts illustrated:
//   - PoolRouter extends SelfSupervisingActor — routees are supervised children
//   - GroupRouter extends EventBasedActor — routees are external references
//   - IRoutingLogic: RoundRobin, Random, ConsistentHashing, SmallestMailbox
//   - system.spawn<PoolRouter>(logic, factory, pool_size)
//   - system.spawn<GroupRouter>(logic, service_key)
//   - router->broadcast(msg)
//   - router->resize(new_size)
//   - router->set_routing_logic(new_logic)
//   - router->add_routee(ref) / router->remove_routee(addr)
//
// =============================================================================

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/routing/group_router.hpp>
#include <hpactor/actor/routing/pool_router.hpp>
#include <hpactor/actor/routing/routing_logic.hpp>
#include <hpactor/config/actor_factory.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/supervision/supervision.hpp>

#include <chrono>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

// ── Custom message type tags ────────────────────────────────────────────────

static const hpactor::TypeTag WorkMsgTag{0x00002000};
static const hpactor::TypeTag ShardMsgTag{0x00002001};
static const hpactor::TypeTag BcastMsgTag{0x00002002};
static const hpactor::TypeTag ResizeMsgTag{0x00002003};
static const hpactor::TypeTag PoisonMsgTag{0x00002004};

// ── String message helpers ──────────────────────────────────────────────────

static hpactor::TypedMessage
make_string_msg(hpactor::TypeTag tag, const std::string& text) {
    hpactor::StreamBuffer payload(text.begin(), text.end());
    return hpactor::TypedMessage(tag, std::move(payload));
}

static std::string extract_string(const hpactor::StreamBuffer& payload) {
    return {payload.begin(), payload.end()};
}

// ── Convenience: send from main thread to an actor ──────────────────────────

static void
send_from_main(hpactor::ActorSystem& system, hpactor::ActorAddress target,
               hpactor::TypeTag tag, const std::string& text) {
    system.deliver_local(target.id, make_string_msg(tag, text));
}

// =============================================================================
// WorkerActor — a simple worker that prints received messages
// =============================================================================

class WorkerActor : public hpactor::EventBasedActor {
  public:
    WorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                std::string name = "worker")
        : hpactor::EventBasedActor(ctx, sys), name_(std::move(name)) {
        become(make_behavior());
    }

    const std::string& name() const {
        return name_;
    }
    int handled() const {
        return handled_;
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            auto text = extract_string(msg.payload());
            std::cout << "  [" << name_ << " id=" << id().value()
                      << "] received \"" << text
                      << "\" (tag=" << static_cast<uint32_t>(msg.type_id())
                      << ")" << std::endl;
            ++handled_;
        }};
    }

  private:
    std::string name_;
    int handled_ = 0;
};

// =============================================================================
// FailingWorkerActor — a worker that fails after N messages
// =============================================================================

class FailingWorkerActor : public hpactor::EventBasedActor,
                           public hpactor::LifecycleActor {
  public:
    FailingWorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                       int fail_after_n = 3)
        : hpactor::EventBasedActor(ctx, sys), fail_after_(fail_after_n) {
        become(make_behavior());
    }

    hpactor::LifecycleActor* as_lifecycle() override {
        return this;
    }
    const hpactor::LifecycleActor* as_lifecycle() const override {
        return this;
    }

    int handled() const {
        return handled_;
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            ++handled_;
            auto text = extract_string(msg.payload());
            std::cout << "  [failing-worker id=" << id().value() << "] received \""
                      << text << "\" (msg #" << handled_ << ")" << std::endl;

            if (handled_ >= fail_after_) {
                std::cout << "  [failing-worker id=" << id().value()
                          << "] FAILING after " << handled_ << " messages"
                          << std::endl;
                set_exit_reason(42);
                transition(hpactor::LifecycleState::kFailed);
            }
        }};
    }

  private:
    int fail_after_;
    int handled_ = 0;
};

// =============================================================================
// ShardActor — processes messages keyed by shard, used with consistent hashing
// =============================================================================

class ShardActor : public hpactor::EventBasedActor {
  public:
    ShardActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
               int shard_id = 0)
        : hpactor::EventBasedActor(ctx, sys), shard_id_(shard_id) {
        become(make_behavior());
    }

    int shard_id() const {
        return shard_id_;
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            auto text = extract_string(msg.payload());
            std::cout << "  [shard-" << shard_id_ << " id=" << id().value()
                      << "] received \"" << text << "\"" << std::endl;
        }};
    }

  private:
    int shard_id_;
};

// =============================================================================
// Demo 1: PoolRouter + RoundRobinLogic — uniform work distribution
// =============================================================================

static void demo_1_round_robin_pool(hpactor::ActorSystem& system) {
    std::cout << "\n--- Demo 1: PoolRouter + RoundRobinLogic ---" << std::endl;

    auto router = system.spawn<hpactor::routing::PoolRouter>(
        std::make_unique<hpactor::routing::RoundRobinLogic>(),
        [](hpactor::ActorContext* ctx,
           hpactor::ActorSystem& sys) -> std::shared_ptr<hpactor::AbstractActor> {
            return std::make_shared<WorkerActor>(ctx, sys, "rr-worker");
        },
        3, // pool of 3 workers
        hpactor::SupervisionPolicy{});

    std::cout << "  Created PoolRouter (id=" << router.id().value()
              << ") with 3 workers, RoundRobinLogic" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send 6 messages — RoundRobinLogic distributes [0,1,2,0,1,2]
    for (int i = 1; i <= 6; ++i) {
        std::ostringstream oss;
        oss << "job-" << i;
        send_from_main(system, router.address(), WorkMsgTag, oss.str());
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Extract and show the router pointer for inspection
    auto* pool = static_cast<hpactor::routing::PoolRouter*>(router.get().get());
    std::cout << "  PoolRouter has " << pool->routee_count() << " routees"
              << std::endl;
}

// =============================================================================
// Demo 2: PoolRouter + ConsistentHashingLogic — sticky key routing
// =============================================================================

static void demo_2_consistent_hashing_pool(hpactor::ActorSystem& system) {
    std::cout << "\n--- Demo 2: PoolRouter + ConsistentHashingLogic ---"
              << std::endl;

    // Custom key extractor: hash on the message TypeTag (the default).
    // Messages with the same TypeTag always go to the same shard.
    auto router = system.spawn<hpactor::routing::PoolRouter>(
        std::make_unique<hpactor::routing::ConsistentHashingLogic>(),
        [i = 0](hpactor::ActorContext* ctx, hpactor::ActorSystem& sys) mutable
            -> std::shared_ptr<hpactor::AbstractActor> {
            return std::make_shared<ShardActor>(ctx, sys, i++);
        },
        3, // 3 shards
        hpactor::SupervisionPolicy{});

    std::cout << "  Created PoolRouter (id=" << router.id().value()
              << ") with 3 shards, ConsistentHashingLogic (128 vnodes)"
              << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send messages with two different TypeTags. Messages with the same
    // TypeTag should consistently land on the same routee.
    // TypeTag ShardMsgTag → always goes to the same shard
    // TypeTag WorkMsgTag  → always goes to the same shard (possibly different)
    send_from_main(system, router.address(), ShardMsgTag, "shard-msg-1");
    send_from_main(system, router.address(), ShardMsgTag, "shard-msg-2");
    send_from_main(system, router.address(), ShardMsgTag, "shard-msg-3");
    send_from_main(system, router.address(), WorkMsgTag, "cross-msg-1");
    send_from_main(system, router.address(), WorkMsgTag, "cross-msg-2");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "  All ShardMsgTag messages went to the same shard; WorkMsgTag"
              << " messages to another (consistent hashing)" << std::endl;
}

// =============================================================================
// Demo 3: Broadcast — send to all routees
// =============================================================================

static void demo_3_broadcast(hpactor::ActorSystem& system) {
    std::cout << "\n--- Demo 3: Broadcast to all routees ---" << std::endl;

    auto router = system.spawn<hpactor::routing::PoolRouter>(
        std::make_unique<hpactor::routing::RoundRobinLogic>(),
        [](hpactor::ActorContext* ctx,
           hpactor::ActorSystem& sys) -> std::shared_ptr<hpactor::AbstractActor> {
            return std::make_shared<WorkerActor>(ctx, sys, "bcast-w");
        },
        3, hpactor::SupervisionPolicy{});

    std::cout << "  Created PoolRouter (id=" << router.id().value()
              << ") with 3 workers" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send a broadcast — all 3 workers should receive the message
    auto* pool = static_cast<hpactor::routing::PoolRouter*>(router.get().get());
    pool->broadcast(make_string_msg(BcastMsgTag, "system-restart-imminent"));

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "  All 3 workers received the broadcast message" << std::endl;
}

// =============================================================================
// Demo 4: Resize pool at runtime
// =============================================================================

static void demo_4_resize_pool(hpactor::ActorSystem& system) {
    std::cout << "\n--- Demo 4: Pool resize at runtime ---" << std::endl;

    auto router = system.spawn<hpactor::routing::PoolRouter>(
        std::make_unique<hpactor::routing::RoundRobinLogic>(),
        [](hpactor::ActorContext* ctx,
           hpactor::ActorSystem& sys) -> std::shared_ptr<hpactor::AbstractActor> {
            return std::make_shared<WorkerActor>(ctx, sys, "resize-w");
        },
        2, hpactor::SupervisionPolicy{});

    auto* pool = static_cast<hpactor::routing::PoolRouter*>(router.get().get());
    std::cout << "  Initial pool size: " << pool->routee_count() << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Scale up: 2 → 5
    pool->resize(5);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "  After resize(5): " << pool->routee_count() << " routees"
              << std::endl;

    // Send a message — should be routed to one of the 5 workers
    send_from_main(system, router.address(), ResizeMsgTag, "post-resize-msg");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Scale down: 5 → 2
    pool->resize(2);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    std::cout << "  After resize(2): " << pool->routee_count() << " routees"
              << std::endl;
}

// =============================================================================
// Demo 5: Routee failure and supervision restart
// =============================================================================

static void demo_5_routee_failure_restart(hpactor::ActorSystem& system) {
    std::cout << "\n--- Demo 5: Routee failure + supervision restart ---"
              << std::endl;

    // Use OneForOne strategy: only the failed routee is restarted.
    hpactor::SupervisionPolicy policy;
    policy.strategy = hpactor::SupervisionPolicy::Strategy::OneForOne;
    policy.max_restarts = 3;
    policy.restart_interval = std::chrono::milliseconds(5000);

    auto router = system.spawn<hpactor::routing::PoolRouter>(
        std::make_unique<hpactor::routing::RoundRobinLogic>(),
        [](hpactor::ActorContext* ctx,
           hpactor::ActorSystem& sys) -> std::shared_ptr<hpactor::AbstractActor> {
            return std::make_shared<FailingWorkerActor>(ctx, sys, 2); // fail
                                                                      // after 2
                                                                      // msgs
        },
        1, // single routee for clarity
        policy);

    auto* pool = static_cast<hpactor::routing::PoolRouter*>(router.get().get());
    std::cout << "  Created PoolRouter with 1 FailingWorkerActor (fails after"
              << " 2 messages). Policy: OneForOne, max 3 restarts." << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send messages: msg 1 & 2 → handled, msg 3 → failure, msg 4 & 5 →
    // replacement worker handles them
    send_from_main(system, router.address(), WorkMsgTag, "safe-msg-1");
    send_from_main(system, router.address(), WorkMsgTag, "safe-msg-2");
    send_from_main(system, router.address(), PoisonMsgTag, "trigger-fail");
    send_from_main(system, router.address(), WorkMsgTag, "post-restart-1");
    send_from_main(system, router.address(), WorkMsgTag, "post-restart-2");

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    std::cout
        << "  Routee count after failure + restart: " << pool->routee_count()
        << std::endl;
    std::cout << "  PoolRouter auto-replaced the failed routee via supervision"
              << std::endl;
}

// =============================================================================
// Demo 6: Runtime routing logic swap
// =============================================================================

static void demo_6_routing_logic_swap(hpactor::ActorSystem& system) {
    std::cout << "\n--- Demo 6: Runtime routing logic swap ---" << std::endl;

    auto router = system.spawn<hpactor::routing::PoolRouter>(
        std::make_unique<hpactor::routing::RoundRobinLogic>(),
        [](hpactor::ActorContext* ctx,
           hpactor::ActorSystem& sys) -> std::shared_ptr<hpactor::AbstractActor> {
            return std::make_shared<WorkerActor>(ctx, sys, "swap-w");
        },
        3, hpactor::SupervisionPolicy{});

    auto* pool = static_cast<hpactor::routing::PoolRouter*>(router.get().get());
    std::cout << "  Initial strategy: " << pool->routing_logic()->name()
              << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send 3 messages with RoundRobin
    send_from_main(system, router.address(), WorkMsgTag, "rr-1");
    send_from_main(system, router.address(), WorkMsgTag, "rr-2");
    send_from_main(system, router.address(), WorkMsgTag, "rr-3");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Swap to RandomLogic at runtime
    pool->set_routing_logic(std::make_unique<hpactor::routing::RandomLogic>(42));
    std::cout << "  Swapped strategy to: " << pool->routing_logic()->name()
              << " (seed=42)" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send 3 more messages — now distributed randomly
    send_from_main(system, router.address(), WorkMsgTag, "rand-1");
    send_from_main(system, router.address(), WorkMsgTag, "rand-2");
    send_from_main(system, router.address(), WorkMsgTag, "rand-3");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::cout << "  Messages after swap use RandomLogic distribution" << std::endl;
}

// =============================================================================
// Demo 7: GroupRouter with external routees
// =============================================================================

static void demo_7_group_router(hpactor::ActorSystem& system) {
    std::cout << "\n--- Demo 7: GroupRouter with external routees ---" << std::endl;

    // Create a GroupRouter with SmallestMailboxLogic (load-aware routing).
    auto router = system.spawn<hpactor::routing::GroupRouter>(
        std::make_unique<hpactor::routing::SmallestMailboxLogic>(),
        "notification-service" // service key for discovery
    );

    std::cout << "  Created GroupRouter (id=" << router.id().value()
              << ", service_key=notification-service, SmallestMailboxLogic)"
              << std::endl;

    // Spawn 3 independent notification actors and register them as routees.
    auto notifier_a = system.spawn<WorkerActor>("notifier-A");
    auto notifier_b = system.spawn<WorkerActor>("notifier-B");
    auto notifier_c = system.spawn<WorkerActor>("notifier-C");

    auto* group = static_cast<hpactor::routing::GroupRouter*>(router.get().get());
    group->add_routee(hpactor::ActorRef(hpactor::Actor(notifier_a)));
    group->add_routee(hpactor::ActorRef(hpactor::Actor(notifier_b)));
    group->add_routee(hpactor::ActorRef(hpactor::Actor(notifier_c)));

    std::cout << "  Registered 3 notification actors as routees" << std::endl;
    std::cout << "  Group routee count: " << group->routee_count() << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Send messages through the group. SmallestMailboxLogic picks the
    // routee with the smallest mailbox depth.
    send_from_main(system, router.address(), WorkMsgTag, "notification-alpha");
    send_from_main(system, router.address(), WorkMsgTag, "notification-beta");
    send_from_main(system, router.address(), WorkMsgTag, "notification-gamma");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Demonstrate dynamic removal
    group->remove_routee(notifier_c.address());
    std::cout << "  Removed notifier-C. Routee count: " << group->routee_count()
              << std::endl;

    // Send one more — only goes to A or B
    send_from_main(system, router.address(), WorkMsgTag, "notification-delta");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}

// =============================================================================
// routing_logic_name helper — expose the current strategy name for display
// =============================================================================

// (This accessor is provided by PoolRouter::routing_logic() -> name()
//  and GroupRouter similarly. We use it in demos above via pool->...)

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 14: Actor Routing ===" << std::endl;
    std::cout << "PoolRouter, GroupRouter, Broadcast, Resize, Supervision,"
              << " Strategy Swap\n"
              << std::endl;

    hpactor::Config config{.scheduler_threads = 2,
                           .max_queue_depth = 1024,
                           .cli = {},
                           .mailbox = {},
                           .dead_letters = {},
                           .tracing = {},
                           .process = {}};
    hpactor::ActorSystem system(config);

    std::cout << "ActorSystem started with " << config.scheduler_threads
              << " scheduler threads\n"
              << std::endl;

    // ── Run all demos ──────────────────────────────────────────────────

    demo_1_round_robin_pool(system);
    demo_2_consistent_hashing_pool(system);
    demo_3_broadcast(system);
    demo_4_resize_pool(system);
    demo_5_routee_failure_restart(system);
    demo_6_routing_logic_swap(system);
    demo_7_group_router(system);

    // ── Done ───────────────────────────────────────────────────────────

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
