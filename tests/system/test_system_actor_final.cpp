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

// System test: Actor Final — branch coverage for behavior swapping,
// typed actor dispatch, proto-stateful request/response, blocking receive,
// daemon run_once, dense computing dispatch, and external gateway route table.

#include <gtest/gtest.h>

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/blocking_actor.hpp>
#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/actor/dense_computing_actor.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/gateway/external_msg_gateway.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_state.hpp>
#include <hpactor/actor/proto_stateful_actor.hpp>
#include <hpactor/actor/stateful_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/actor/typed_actor.hpp>
#include <hpactor/actor/typed_behavior.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/msg/typed_message.hpp>

#include "system_test_fixture.hpp"

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace hpactor;

using CountingActor = test::CountingActor;
HPACTOR_REGISTER_ACTOR("CountingActor", CountingActor);

// Helper to create a TypedMessage with just a tag for local-only tests
static TypedMessage make_test_msg(uint32_t tag_val) {
    StreamBuffer empty;
    return TypedMessage(static_cast<TypeTag>(tag_val), std::move(empty));
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 1: Actor with behavior swapping (become)
// ═══════════════════════════════════════════════════════════════════════════════

// Actor that swaps behaviors on demand
class BehaviorSwapActor : public EventBasedActor, public LifecycleActor {
  public:
    enum class Phase { Initial, Swapped, Empty };
    Phase current_phase = Phase::Initial;
    int msg_count = 0;

    BehaviorSwapActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    void swap_to_second() {
        become(Behavior{[this](TypedMessage& /*msg*/) {
            current_phase = Phase::Swapped;
            msg_count++;
        }});
    }

    void become_noop() {
        become_empty();
        current_phase = Phase::Empty;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& /*msg*/) {
            current_phase = Phase::Initial;
            msg_count++;
        }};
    }
};

HPACTOR_REGISTER_ACTOR("BehaviorSwapActor", BehaviorSwapActor);

TEST(ActorFinal, BehaviorSwapBecome) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto handle = system.spawn<BehaviorSwapActor>();
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<BehaviorSwapActor*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    // Transition to active so receive works
    if (auto* lc = actor->as_lifecycle()) {
        if (lc->state() != LifecycleState::kActive) {
            lc->transition(LifecycleState::kActive);
        }
    }

    EXPECT_EQ(actor->current_phase, BehaviorSwapActor::Phase::Initial);

    // Send a message — should be handled by initial behavior
    auto msg1 = make_test_msg(0x1000);
    actor->receive(msg1);
    EXPECT_EQ(actor->current_phase, BehaviorSwapActor::Phase::Initial);
    EXPECT_EQ(actor->msg_count, 1);

    // Swap behavior
    actor->swap_to_second();

    // Send another message — should be handled by swapped behavior
    auto msg2 = make_test_msg(0x2000);
    actor->receive(msg2);
    EXPECT_EQ(actor->current_phase, BehaviorSwapActor::Phase::Swapped);
    EXPECT_EQ(actor->msg_count, 2);

    // Become empty — messages should be dropped
    actor->become_noop();
    EXPECT_EQ(actor->current_phase, BehaviorSwapActor::Phase::Empty);

    auto msg3 = make_test_msg(0x3000);
    actor->receive(msg3);
    // msg_count should NOT increment — empty behavior drops messages
    EXPECT_EQ(actor->msg_count, 2);

    // Mark actor as stopped before shutdown to avoid hang
    if (auto* lc = actor->as_lifecycle()) {
        lc->transition(LifecycleState::kStopping);
        lc->transition(LifecycleState::kStopped);
    }

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 2: Actor state after multiple lifecycle transitions
// ═══════════════════════════════════════════════════════════════════════════════

class LifecycleTransitionActor : public EventBasedActor, public LifecycleActor {
  public:
    std::vector<LifecycleState> state_history;
    int lifecycle_count = 0;

    LifecycleTransitionActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    void on_start() override {
        state_history.push_back(LifecycleState::kActive);
        lifecycle_count++;
    }

    void on_stop() override {
        state_history.push_back(LifecycleState::kStopping);
        lifecycle_count++;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& /*msg*/) { lifecycle_count++; }};
    }
};

HPACTOR_REGISTER_ACTOR("LifecycleTransitionActor", LifecycleTransitionActor);

TEST(ActorFinal, ActorStateAfterMultipleLifecycleTransitions) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto handle = system.spawn<LifecycleTransitionActor>();
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<LifecycleTransitionActor*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    auto* lifecycle = actor->as_lifecycle();
    ASSERT_NE(lifecycle, nullptr);

    // Force to kActive for consistent testing
    if (lifecycle->state() != LifecycleState::kActive) {
        lifecycle->transition(LifecycleState::kActive);
    }
    EXPECT_EQ(lifecycle->state(), LifecycleState::kActive);

    // Send a message while active
    auto msg = make_test_msg(0x42);
    actor->receive(msg);
    EXPECT_GE(actor->lifecycle_count, 1);

    // Transition to kFailed (valid from kActive)
    lifecycle->transition(LifecycleState::kFailed);
    EXPECT_EQ(lifecycle->state(), LifecycleState::kFailed);

    // Transition to kStarting (restart recovery from kFailed)
    lifecycle->transition(LifecycleState::kStarting);
    EXPECT_EQ(lifecycle->state(), LifecycleState::kStarting);

    // Transition to kActive again
    lifecycle->transition(LifecycleState::kActive);
    EXPECT_EQ(lifecycle->state(), LifecycleState::kActive);

    // Transition through drain → stop path
    lifecycle->transition(LifecycleState::kDraining);
    EXPECT_EQ(lifecycle->state(), LifecycleState::kDraining);

    lifecycle->transition(LifecycleState::kStopping);
    EXPECT_EQ(lifecycle->state(), LifecycleState::kStopping);

    lifecycle->transition(LifecycleState::kStopped);
    EXPECT_EQ(lifecycle->state(), LifecycleState::kStopped);

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 3: TypedActor message dispatch
// ═══════════════════════════════════════════════════════════════════════════════

// Define message types for typed actor
struct PingMsg {
    int sequence = 0;
};
struct PongReply {
    int sequence = 0;
};

using PingPongActor =
    TypedEventBasedActor<result<int>(PingMsg), result<void>(PongReply)>;

class PingPongActorImpl : public PingPongActor {
  public:
    int ping_count = 0;
    int pong_count = 0;

    PingPongActorImpl(ActorContext* ctx, ActorSystem& sys)
        : PingPongActor(ctx, sys) {
        become(make_behavior());
    }

    behavior_type make_behavior() override {
        behavior_type bh;
        bh.on([](PingMsg msg) -> int { return msg.sequence + 1; });
        bh.on([this](PongReply msg) {
            pong_count++;
            (void)msg;
        });
        return bh;
    }

    // Accessor for typed dispatch
    result<int> handle_ping(PingMsg msg) {
        return (*this)(std::move(msg));
    }
};

HPACTOR_REGISTER_ACTOR("PingPongActorImpl", PingPongActorImpl);

TEST(ActorFinal, TypedActorMessageDispatch) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto handle = system.spawn<PingPongActorImpl>();
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<PingPongActorImpl*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    // Typed dispatch via operator()
    PingMsg ping{42};
    auto r = actor->handle_ping(ping);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 43);

    // TypedBehavior factory methods
    TypedBehavior<result<int>(PingMsg)> bh;
    bh.on([](PingMsg msg) -> int { return msg.sequence * 2; });

    // Create TypedEventBasedActorRef handle
    auto ref =
        TypedEventBasedActorRef<result<int>(PingMsg), result<void>(PongReply)>(
            std::static_pointer_cast<PingPongActor>(handle.get()));
    EXPECT_TRUE(static_cast<bool>(ref));
    EXPECT_EQ(ref.id(), handle.id());
    EXPECT_EQ(ref.address(), handle.address());

    // Send via typed ref
    ref(PingMsg{10});
    ref(PongReply{99});

    auto shutdown_result = system.shutdown();
    EXPECT_TRUE(shutdown_result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 4: ProtoStatefulActor request/response
// ═══════════════════════════════════════════════════════════════════════════════

struct CalculatorState {
    int last_result = 0;
    int operation_count = 0;
};

class CalculatorActor : public ProtoStatefulActor<CalculatorState> {
  public:
    using ProtoStatefulActor::ProtoStatefulActor;

    int get_last_result() const {
        return state().last_result;
    }
    int get_operation_count() const {
        return state().operation_count;
    }

  protected:
    void register_handlers() override {
        // Test that register_handlers is called and state is accessible
        state().last_result = 0;
        state().operation_count = 0;
    }
};

HPACTOR_REGISTER_ACTOR("CalculatorActor", CalculatorActor);

TEST(ActorFinal, ProtoStatefulActorStateAccess) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto handle = system.spawn<CalculatorActor>();
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<CalculatorActor*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    // State should be accessible after spawn
    EXPECT_EQ(actor->get_last_result(), 0);
    EXPECT_EQ(actor->get_operation_count(), 0);

    // Manually modify state via mutable access
    actor->state().last_result = 42;
    actor->state().operation_count = 3;

    EXPECT_EQ(actor->get_last_result(), 42);
    EXPECT_EQ(actor->get_operation_count(), 3);

    // Verify it's an EventBasedActor
    EXPECT_TRUE(actor->is_event_based_actor());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 5: BlockingActor receive loop
// ═══════════════════════════════════════════════════════════════════════════════

class ReceiveLoopActor : public BlockingActor {
  public:
    std::atomic<int> received{0};
    std::atomic<bool> running_flag{false};

    ReceiveLoopActor(ActorContext* ctx, ActorSystem& sys)
        : BlockingActor(ctx, sys) {}

    void on_activate() override {
        running_flag.store(true);
        BlockingActor::on_activate();
    }

    void receive(TypedMessage& /*msg*/) override {
        received.fetch_add(1);
    }
};

HPACTOR_REGISTER_ACTOR("ReceiveLoopActor", ReceiveLoopActor);

TEST(ActorFinal, BlockingActorReceiveLoop) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto handle = system.spawn<ReceiveLoopActor>();
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<ReceiveLoopActor*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    // BlockingActor uses DedicatedThread dispatch
    EXPECT_EQ(actor->dispatch_policy(), sched::DispatchPolicy::DedicatedThread);

    // Verify the actor was constructed and can be shut down
    // The dedicated thread is started by the scheduler
    EXPECT_NE(actor->address().id, ActorId{0});

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 6: DaemonActor run_once cycle
// ═══════════════════════════════════════════════════════════════════════════════

class CycleDaemon : public DaemonActor {
  public:
    std::atomic<int> iterations{0};
    std::atomic<bool> started{false};
    std::atomic<bool> stopped{false};
    int max_iterations = 5;

    CycleDaemon(ActorContext* ctx, ActorSystem& sys, int max_iter = 5)
        : DaemonActor(ctx, sys), max_iterations(max_iter) {}

    bool run_once() override {
        started.store(true);
        iterations.fetch_add(1);
        if (iterations.load() >= max_iterations)
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }

    void on_daemon_start() override {
        started.store(true);
    }
    void on_daemon_stop() override {
        stopped.store(true);
    }
};

HPACTOR_REGISTER_ACTOR("CycleDaemon", CycleDaemon);

TEST(ActorFinal, DaemonActorRunOnceCycle) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto handle = system.spawn<CycleDaemon>(/*max_iter=*/3);
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<CycleDaemon*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    // DaemonActor uses DedicatedThread dispatch
    EXPECT_EQ(actor->dispatch_policy(), sched::DispatchPolicy::DedicatedThread);

    // Wait for the daemon loop to complete
    test::assert_eventually([&actor]() { return actor->iterations.load() >= 3; });

    EXPECT_GE(actor->iterations.load(), 3);
    EXPECT_TRUE(actor->started.load());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 7: DenseComputingActor dispatch
// ═══════════════════════════════════════════════════════════════════════════════

class DenseProbeActor : public DenseComputingActor {
  public:
    std::atomic<int> msg_count{0};

    DenseProbeActor(ActorContext* ctx, ActorSystem& sys, uint32_t pool_size = 2)
        : DenseComputingActor(ctx, sys, pool_size) {}

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& /*msg*/) { msg_count.fetch_add(1); }};
    }
};

HPACTOR_REGISTER_ACTOR("DenseProbeActor", DenseProbeActor);

TEST(ActorFinal, DenseComputingActorDispatch) {
    Config cfg = test::config_with_scheduler(1);
    ActorSystem system(cfg);

    auto handle = system.spawn<DenseProbeActor>(/*pool_size=*/4);
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<DenseProbeActor*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    // Verify dispatch policy and hints
    EXPECT_EQ(actor->dispatch_policy(), sched::DispatchPolicy::DedicatedPool);
    EXPECT_EQ(actor->pool_size(), 4u);

    auto hints = actor->dispatch_hints();
    EXPECT_EQ(hints.pool_size, 4u);

    // Actor construction and policy verification are sufficient for coverage
    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 8: ExternalMsgGatewayActor route table
// ═══════════════════════════════════════════════════════════════════════════════

class RouteTestGateway : public ExternalMsgGatewayActor, public LifecycleActor {
  public:
    RouteTestGateway(ActorContext* ctx, ActorSystem& sys)
        : ExternalMsgGatewayActor(ctx, sys) {}

    LifecycleActor* as_lifecycle() override {
        return this;
    }
    const LifecycleActor* as_lifecycle() const override {
        return this;
    }

    // Override to NOT start daemon thread — we only test the route table
    void on_activate() override {
        // Skip DaemonActor::on_activate() which starts the daemon thread
    }

    bool run_once() override {
        return true;
    }

    // Expose protected methods for testing
    ActorAddr resolve(const std::string& path) {
        return resolve_route(path);
    }

    TypedMessage apply_transform(TypeTag tag, StreamBuffer payload) {
        return transform(tag, std::move(payload));
    }
};

HPACTOR_REGISTER_ACTOR("RouteTestGateway", RouteTestGateway);

TEST(ActorFinal, ExternalMsgGatewayRouteTable) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    // Spawn target actors for routing addresses
    auto target1 = system.spawn<CountingActor>();
    auto target2 = system.spawn<CountingActor>();

    auto gw_handle = system.spawn<RouteTestGateway>();
    ASSERT_TRUE(gw_handle.get() != nullptr);
    auto* gw = static_cast<RouteTestGateway*>(gw_handle.get().get());

    // Register routes via ActorAddr
    gw->route("/api/v1/users", target1.address());
    gw->route("/api/v1/orders", target2.address());
    gw->route("/api/v1/", target1.address()); // prefix route

    // Exact match
    auto resolved = gw->resolve("/api/v1/users");
    EXPECT_EQ(resolved.id, target1.id());

    resolved = gw->resolve("/api/v1/orders");
    EXPECT_EQ(resolved.id, target2.id());

    // Prefix match
    resolved = gw->resolve("/api/v1/products");
    EXPECT_EQ(resolved.id, target1.id());

    // No match
    resolved = gw->resolve("/unknown/path");
    EXPECT_EQ(resolved, invalid_actor_addr);

    // Route via ActorRef
    auto target3 = system.spawn<CountingActor>();
    gw->route("/api/v2/items", ActorRef(target3));

    resolved = gw->resolve("/api/v2/items");
    EXPECT_EQ(resolved.id, target3.id());

    // No-transform path
    auto tag = static_cast<TypeTag>(0x42);
    StreamBuffer payload;
    auto msg = gw->apply_transform(tag, payload);
    EXPECT_EQ(msg.type_id(), tag);

    // Set a custom transform
    gw->set_transform(tag, [](StreamBuffer data) -> TypedMessage {
        return TypedMessage(static_cast<TypeTag>(0x99), std::move(data));
    });

    StreamBuffer payload2;
    auto transformed = gw->apply_transform(tag, payload2);
    EXPECT_EQ(transformed.type_id(), static_cast<TypeTag>(0x99));

    // DaemonActor interface
    EXPECT_EQ(gw->dispatch_policy(), sched::DispatchPolicy::DedicatedThread);

    // Stop all actors before shutdown to avoid hang
    auto stop_actor = [](Actor& a) {
        if (auto* raw = a.get().get()) {
            if (auto* lc = raw->as_lifecycle()) {
                lc->transition(LifecycleState::kStopping);
                lc->transition(LifecycleState::kStopped);
            }
        }
    };
    stop_actor(target1);
    stop_actor(target2);
    stop_actor(target3);
    if (auto* lc = gw->as_lifecycle()) {
        lc->transition(LifecycleState::kStopping);
        lc->transition(LifecycleState::kStopped);
    }

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 9: StatefulActor with explicit state management
// ═══════════════════════════════════════════════════════════════════════════════

struct CounterState {
    int count = 0;
    std::string last_action;
};

class StatefulCounterActor : public StatefulActor<CounterState> {
  public:
    StatefulCounterActor(ActorContext* ctx, ActorSystem& sys)
        : StatefulActor<CounterState>(ctx, sys) {
        become(make_behavior());
    }

    int value() const {
        return state().count;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            state().count++;
            state().last_action =
                "message_type_" +
                std::to_string(static_cast<uint32_t>(msg.type_id()));
        }};
    }
};

HPACTOR_REGISTER_ACTOR("StatefulCounterActor", StatefulCounterActor);

TEST(ActorFinal, StatefulActorStateManagement) {
    Config cfg = test::config_with_scheduler(0);
    ActorSystem system(cfg);

    auto handle = system.spawn<StatefulCounterActor>();
    ASSERT_TRUE(handle.get() != nullptr);
    auto* actor = static_cast<StatefulCounterActor*>(handle.get().get());
    ASSERT_NE(actor, nullptr);

    // Initial state
    EXPECT_EQ(actor->value(), 0);

    // Send messages to increment state
    auto msg1 = make_test_msg(0x1001);
    actor->receive(msg1);
    EXPECT_EQ(actor->value(), 1);

    auto msg2 = make_test_msg(0x1002);
    actor->receive(msg2);
    EXPECT_EQ(actor->value(), 2);

    auto msg3 = make_test_msg(0x1003);
    actor->receive(msg3);
    EXPECT_EQ(actor->value(), 3);

    // State persists across handler invocations
    EXPECT_EQ(actor->state().count, 3);
    EXPECT_FALSE(actor->state().last_action.empty());

    auto result = system.shutdown();
    EXPECT_TRUE(result.has_value());
}

// ═══════════════════════════════════════════════════════════════════════════════
// Test 10: Behavior combinator API (intercept, compose, setup)
// ═══════════════════════════════════════════════════════════════════════════════

TEST(ActorFinal, BehaviorCombinatorAPI) {
    // Behavior::make() factory
    auto bh = Behavior::make();
    EXPECT_FALSE(static_cast<bool>(bh)); // empty

    // Behavior::receive factory
    int raw_count = 0;
    auto raw_bh = Behavior::receive([&raw_count](TypedMessage&) { raw_count++; });
    EXPECT_TRUE(static_cast<bool>(raw_bh));

    auto msg1 = make_test_msg(0x1001);
    raw_bh(msg1);
    EXPECT_EQ(raw_count, 1);

    // Behavior::empty factory
    auto empty_bh = Behavior::empty();
    EXPECT_FALSE(static_cast<bool>(empty_bh));

    // Behavior::intercept combinator
    int before = 0;
    int after = 0;
    auto inner = Behavior::receive([&after](TypedMessage&) { after++; });
    auto intercepted = Behavior::intercept(
        inner, [&before](TypedMessage& msg, Behavior::next_fn next) {
            before++;
            next(msg);
        });

    auto msg2 = make_test_msg(0x1002);
    intercepted(msg2);
    EXPECT_EQ(before, 1);
    EXPECT_EQ(after, 1);

    // Behavior::compose combinator
    int first_count = 0;
    int second_count = 0;
    auto first_bh =
        Behavior::receive([&first_count](TypedMessage&) { first_count++; });
    auto second_bh =
        Behavior::receive([&second_count](TypedMessage&) { second_count++; });
    auto composed = Behavior::compose(first_bh, second_bh);

    auto msg3 = make_test_msg(0x1003);
    composed(msg3);
    EXPECT_EQ(first_count, 1);
    EXPECT_EQ(second_count, 1);

    // Behavior::setup (lazy factory)
    bool factory_called = false;
    auto setup_bh = Behavior::setup([&factory_called]() -> Behavior {
        factory_called = true;
        return Behavior::receive([](TypedMessage&) {});
    });

    // Before first message, factory not called
    EXPECT_FALSE(factory_called);

    auto msg4 = make_test_msg(0x1004);
    setup_bh(msg4);
    EXPECT_TRUE(factory_called);

    // Behavior::on_signal combinator
    int signal_count = 0;
    int other_count = 0;
    auto inner2 =
        Behavior::receive([&other_count](TypedMessage&) { other_count++; });

    auto signal_bh = Behavior::on_signal(
        static_cast<TypeTag>(0x100),
        [&signal_count](TypedMessage&) { signal_count++; }, inner2);

    auto signal_msg = make_test_msg(0x100);
    signal_bh(signal_msg);
    EXPECT_EQ(signal_count, 1);
    EXPECT_EQ(other_count, 0); // signal consumed, not forwarded

    auto normal_msg = make_test_msg(0x200);
    signal_bh(normal_msg);
    EXPECT_EQ(signal_count, 1); // unchanged
    EXPECT_EQ(other_count, 1);  // forwarded to inner
}
