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

#include <gtest/gtest.h>

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/fsm/behavior_fsm_builder.hpp>
#include <hpactor/actor/fsm/fsm_directive.hpp>
#include <hpactor/actor/fsm/fsm_runtime.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <string>
#include <utility>
#include <vector>

namespace hpactor {
namespace {

// ── Test FSM types ──────────────────────────────────────────

enum class DoorState { Closed, Opened, Locked };
struct DoorData {
    int open_count = 0;
    int close_count = 0;
    bool operator==(const DoorData&) const = default;
};
using DoorDirective = FsmDirective<DoorState, DoorData>;

// ── Helper: construct a simple TypedMessage for testing ──────────

TypedMessage make_test_msg(TypeTag tag = static_cast<TypeTag>(1)) {
    StreamBuffer payload;
    payload.resize(4, 0);
    return TypedMessage(tag, std::move(payload));
}

// ═════════════════════════════════════════════════════════════
// FsmDirective static factories
// ═════════════════════════════════════════════════════════════

TEST(FsmDirectiveTest, StayCreatesStayDirective) {
    auto d = DoorDirective::stay();
    EXPECT_EQ(d.kind, DoorDirective::Kind::Stay);
    EXPECT_FALSE(d.update_data);
}

TEST(FsmDirectiveTest, StayWithDataUpdatesData) {
    DoorData new_data{42, 7};
    auto d = DoorDirective::stay(new_data);
    EXPECT_EQ(d.kind, DoorDirective::Kind::Stay);
    EXPECT_TRUE(d.update_data);
    EXPECT_EQ(d.target_data.open_count, 42);
    EXPECT_EQ(d.target_data.close_count, 7);
}

TEST(FsmDirectiveTest, GoToCreatesGoToDirective) {
    auto d = DoorDirective::go_to(DoorState::Opened);
    EXPECT_EQ(d.kind, DoorDirective::Kind::GoTo);
    EXPECT_EQ(d.target_state, DoorState::Opened);
    EXPECT_FALSE(d.update_data);
}

TEST(FsmDirectiveTest, GoToWithDataCarriesData) {
    DoorData new_data{1, 2};
    auto d = DoorDirective::go_to(DoorState::Locked, new_data);
    EXPECT_EQ(d.kind, DoorDirective::Kind::GoTo);
    EXPECT_EQ(d.target_state, DoorState::Locked);
    EXPECT_EQ(d.target_data.open_count, 1);
}

TEST(FsmDirectiveTest, StopCreatesStopDirective) {
    auto d = DoorDirective::stop();
    EXPECT_EQ(d.kind, DoorDirective::Kind::Stop);
}

// ═════════════════════════════════════════════════════════════
// FsmRuntime dispatch and state transitions
// ═════════════════════════════════════════════════════════════

TEST(FsmRuntimeTest, UnhandledMessageInCurrentStateIsDropped) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    auto msg = make_test_msg(static_cast<TypeTag>(99));
    rt->dispatch(msg);

    EXPECT_EQ(rt->current_state, DoorState::Closed);
    EXPECT_EQ(rt->data.open_count, 0);
    EXPECT_FALSE(rt->stopped);
}

TEST(FsmRuntimeTest, HandlerInCurrentStateIsInvoked) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kOpenTag = static_cast<TypeTag>(100);
    bool handler_called = false;

    rt->handlers[{DoorState::Closed, kOpenTag}] =
        [&](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        handler_called = true;
        return DoorDirective::go_to(DoorState::Opened);
    };

    auto msg = make_test_msg(kOpenTag);
    rt->dispatch(msg);

    EXPECT_TRUE(handler_called);
    EXPECT_EQ(rt->current_state, DoorState::Opened);
}

TEST(FsmRuntimeTest, HandlerNotCalledInWrongState) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kOpenTag = static_cast<TypeTag>(100);
    bool handler_called = false;

    rt->handlers[{DoorState::Opened, kOpenTag}] =
        [&](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        handler_called = true;
        return DoorDirective::stay();
    };

    auto msg = make_test_msg(kOpenTag);
    rt->dispatch(msg);

    EXPECT_FALSE(handler_called);
    EXPECT_EQ(rt->current_state, DoorState::Closed);
}

TEST(FsmRuntimeTest, StayDirectiveKeepsCurrentState) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kPingTag = static_cast<TypeTag>(200);
    rt->handlers[{DoorState::Closed, kPingTag}] =
        [](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        return DoorDirective::stay();
    };

    auto msg = make_test_msg(kPingTag);
    rt->dispatch(msg);

    EXPECT_EQ(rt->current_state, DoorState::Closed);
    EXPECT_FALSE(rt->stopped);
}

TEST(FsmRuntimeTest, StayDirectiveUpdatesData) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kUpdateTag = static_cast<TypeTag>(300);
    rt->handlers[{DoorState::Closed, kUpdateTag}] =
        [](TypedMessage& /*msg*/, DoorData& data) -> DoorDirective {
        DoorData new_data = data;
        new_data.close_count++;
        return DoorDirective::stay(new_data);
    };

    auto msg = make_test_msg(kUpdateTag);
    rt->dispatch(msg);

    EXPECT_EQ(rt->current_state, DoorState::Closed);
    EXPECT_EQ(rt->data.close_count, 1);
}

TEST(FsmRuntimeTest, GoToDirectiveTransitionsState) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kOpenTag = static_cast<TypeTag>(400);
    rt->handlers[{DoorState::Closed, kOpenTag}] =
        [](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        return DoorDirective::go_to(DoorState::Opened);
    };

    auto msg = make_test_msg(kOpenTag);
    rt->dispatch(msg);

    EXPECT_EQ(rt->current_state, DoorState::Opened);
}

TEST(FsmRuntimeTest, GoToDirectiveTransitionsStateWithData) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kOpenTag = static_cast<TypeTag>(500);
    rt->handlers[{DoorState::Closed, kOpenTag}] =
        [](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        DoorData new_data{1, 0};
        return DoorDirective::go_to(DoorState::Opened, new_data);
    };

    auto msg = make_test_msg(kOpenTag);
    rt->dispatch(msg);

    EXPECT_EQ(rt->current_state, DoorState::Opened);
    EXPECT_EQ(rt->data.open_count, 1);
}

TEST(FsmRuntimeTest, StopDirectiveTerminatesFsm) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kStopTag = static_cast<TypeTag>(600);
    rt->handlers[{DoorState::Closed, kStopTag}] =
        [](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        return DoorDirective::stop();
    };

    auto msg = make_test_msg(kStopTag);
    rt->dispatch(msg);

    EXPECT_TRUE(rt->stopped);

    // Subsequent messages should be dropped
    bool second_handler_called = false;
    rt->handlers[{DoorState::Closed, static_cast<TypeTag>(700)}] =
        [&](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        second_handler_called = true;
        return DoorDirective::stay();
    };
    auto msg2 = make_test_msg(static_cast<TypeTag>(700));
    rt->dispatch(msg2);
    EXPECT_FALSE(second_handler_called);
}

TEST(FsmRuntimeTest, MultiStepTransition) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kStep1 = static_cast<TypeTag>(800);
    const TypeTag kStep2 = static_cast<TypeTag>(801);

    rt->handlers[{DoorState::Closed, kStep1}] =
        [](TypedMessage& /*msg*/, DoorData& data) -> DoorDirective {
        DoorData d = data;
        d.open_count++;
        return DoorDirective::go_to(DoorState::Opened, d);
    };
    rt->handlers[{DoorState::Opened, kStep2}] =
        [](TypedMessage& /*msg*/, DoorData& data) -> DoorDirective {
        DoorData d = data;
        d.close_count++;
        return DoorDirective::go_to(DoorState::Closed, d);
    };

    auto msg1 = make_test_msg(kStep1);
    rt->dispatch(msg1);
    EXPECT_EQ(rt->current_state, DoorState::Opened);
    EXPECT_EQ(rt->data.open_count, 1);
    EXPECT_EQ(rt->data.close_count, 0);

    auto msg2 = make_test_msg(kStep2);
    rt->dispatch(msg2);
    EXPECT_EQ(rt->current_state, DoorState::Closed);
    EXPECT_EQ(rt->data.open_count, 1);
    EXPECT_EQ(rt->data.close_count, 1);
}

TEST(FsmRuntimeTest, TransitionHooksAreCalled) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    std::vector<std::pair<DoorState, DoorState>> transitions;
    rt->transition_handlers.push_back(
        [&](DoorState from, DoorState to, DoorData& /*data*/) {
            transitions.emplace_back(from, to);
        });

    const TypeTag kOpenTag = static_cast<TypeTag>(900);
    rt->handlers[{DoorState::Closed, kOpenTag}] =
        [](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        return DoorDirective::go_to(DoorState::Opened);
    };

    auto msg = make_test_msg(kOpenTag);
    rt->dispatch(msg);

    ASSERT_EQ(transitions.size(), 1);
    EXPECT_EQ(transitions[0].first, DoorState::Closed);
    EXPECT_EQ(transitions[0].second, DoorState::Opened);
}

TEST(FsmRuntimeTest, TransitionHookNotCalledOnStay) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    bool hook_called = false;
    rt->transition_handlers.push_back(
        [&](DoorState /*from*/, DoorState /*to*/, DoorData& /*data*/) {
            hook_called = true;
        });

    const TypeTag kPingTag = static_cast<TypeTag>(1000);
    rt->handlers[{DoorState::Closed, kPingTag}] =
        [](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
        return DoorDirective::stay();
    };

    auto msg = make_test_msg(kPingTag);
    rt->dispatch(msg);

    EXPECT_FALSE(hook_called);
}

// ═════════════════════════════════════════════════════════════
// Timeout tests (deterministic — inject timeout directly)
// ═════════════════════════════════════════════════════════════

TEST(FsmTimeoutTest, TimeoutHandlerTransitionsState) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    typename FsmRuntime<DoorState, DoorData>::TimeoutConfig cfg;
    cfg.duration = std::chrono::milliseconds(100);
    cfg.target_state = DoorState::Opened;
    cfg.target_data = DoorData{};
    rt->timeout_configs[DoorState::Closed] = cfg;

    const TypeTag kTimeoutTag = static_cast<TypeTag>(0x7F000001);
    rt->set_timeout_tag(kTimeoutTag);

    StreamBuffer empty;
    TypedMessage timeout_msg(kTimeoutTag, empty);
    rt->dispatch(timeout_msg);

    EXPECT_EQ(rt->current_state, DoorState::Opened);
}

TEST(FsmTimeoutTest, TimeoutFiresTransitionHooks) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    typename FsmRuntime<DoorState, DoorData>::TimeoutConfig cfg;
    cfg.duration = std::chrono::milliseconds(100);
    cfg.target_state = DoorState::Opened;
    cfg.target_data = DoorData{};
    rt->timeout_configs[DoorState::Closed] = cfg;

    const TypeTag kTimeoutTag = static_cast<TypeTag>(0x7F000002);
    rt->set_timeout_tag(kTimeoutTag);

    bool hook_called = false;
    rt->transition_handlers.push_back(
        [&](DoorState from, DoorState to, DoorData& /*data*/) {
            EXPECT_EQ(from, DoorState::Closed);
            EXPECT_EQ(to, DoorState::Opened);
            hook_called = true;
        });

    StreamBuffer empty;
    TypedMessage timeout_msg(kTimeoutTag, empty);
    rt->dispatch(timeout_msg);

    EXPECT_TRUE(hook_called);
    EXPECT_EQ(rt->current_state, DoorState::Opened);
}

TEST(FsmTimeoutTest, TimeoutNotConfiguredIsNoOp) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    const TypeTag kTimeoutTag = static_cast<TypeTag>(0x7F000003);
    rt->set_timeout_tag(kTimeoutTag);

    StreamBuffer empty;
    TypedMessage timeout_msg(kTimeoutTag, empty);
    rt->dispatch(timeout_msg);

    EXPECT_EQ(rt->current_state, DoorState::Closed);
}

TEST(FsmTimeoutTest, TimeoutAfterStopIsIgnored) {
    auto rt = std::make_shared<FsmRuntime<DoorState, DoorData>>(DoorState::Closed,
                                                                DoorData{});

    typename FsmRuntime<DoorState, DoorData>::TimeoutConfig cfg;
    cfg.duration = std::chrono::milliseconds(100);
    cfg.target_state = DoorState::Opened;
    cfg.target_data = DoorData{};
    rt->timeout_configs[DoorState::Closed] = cfg;

    const TypeTag kTimeoutTag = static_cast<TypeTag>(0x7F000004);
    rt->set_timeout_tag(kTimeoutTag);
    rt->stopped = true;

    StreamBuffer empty;
    TypedMessage timeout_msg(kTimeoutTag, empty);
    rt->dispatch(timeout_msg);

    EXPECT_EQ(rt->current_state, DoorState::Closed);
}

TEST(FsmTimeoutTest, ChainedTimeouts) {
    enum class State { A, B, C };
    using CTSDirective = FsmDirective<State, int>;
    (void)CTSDirective{};

    auto rt = std::make_shared<FsmRuntime<State, int>>(State::A, 0);

    {
        typename FsmRuntime<State, int>::TimeoutConfig cfg;
        cfg.duration = std::chrono::milliseconds(100);
        cfg.target_state = State::B;
        cfg.target_data = 1;
        rt->timeout_configs[State::A] = cfg;
    }
    {
        typename FsmRuntime<State, int>::TimeoutConfig cfg;
        cfg.duration = std::chrono::milliseconds(200);
        cfg.target_state = State::C;
        cfg.target_data = 2;
        rt->timeout_configs[State::B] = cfg;
    }

    const TypeTag kTimeoutTag = static_cast<TypeTag>(0x7F000005);
    rt->set_timeout_tag(kTimeoutTag);

    {
        StreamBuffer empty;
        TypedMessage timeout_msg(kTimeoutTag, empty);
        rt->dispatch(timeout_msg);
    }
    EXPECT_EQ(rt->current_state, State::B);
    EXPECT_EQ(rt->data, 1);

    {
        StreamBuffer empty;
        TypedMessage timeout_msg(kTimeoutTag, empty);
        rt->dispatch(timeout_msg);
    }
    EXPECT_EQ(rt->current_state, State::C);
    EXPECT_EQ(rt->data, 2);
}

// ═════════════════════════════════════════════════════════════
// BehaviorFsmBuilder fluent API
// ═════════════════════════════════════════════════════════════

TEST(BehaviorFsmBuilderTest, StartCreatesBuilder) {
    auto builder = BehaviorFsmBuilder<DoorState, DoorData>::start(
        DoorState::Closed, DoorData{});
    auto bh = builder.build(nullptr);
    EXPECT_TRUE(static_cast<bool>(bh));
}

TEST(BehaviorFsmBuilderTest, InStateOnRegistersHandler) {
    const TypeTag kOpenTag = static_cast<TypeTag>(2001);
    bool handler_called = false;

    auto bh =
        BehaviorFsmBuilder<DoorState, DoorData>::start(DoorState::Closed, DoorData{})
            .in_state(DoorState::Closed)
            .on_raw(kOpenTag,
                    [&](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
                        handler_called = true;
                        return DoorDirective::go_to(DoorState::Opened);
                    })
            .build(nullptr);

    auto msg = make_test_msg(kOpenTag);
    bh(msg);
    EXPECT_TRUE(handler_called);
}

TEST(BehaviorFsmBuilderTest, OnTransitionRegistersHook) {
    const TypeTag kOpenTag = static_cast<TypeTag>(2002);
    std::vector<std::pair<DoorState, DoorState>> transitions;

    auto bh =
        BehaviorFsmBuilder<DoorState, DoorData>::start(DoorState::Closed, DoorData{})
            .on_transition([&](DoorState from, DoorState to, DoorData& /*data*/) {
                transitions.emplace_back(from, to);
            })
            .in_state(DoorState::Closed)
            .on_raw(kOpenTag,
                    [](TypedMessage& /*msg*/, DoorData& /*data*/) -> DoorDirective {
                        return DoorDirective::go_to(DoorState::Opened);
                    })
            .build(nullptr);

    auto msg = make_test_msg(kOpenTag);
    bh(msg);

    ASSERT_EQ(transitions.size(), 1);
    EXPECT_EQ(transitions[0].first, DoorState::Closed);
    EXPECT_EQ(transitions[0].second, DoorState::Opened);
}

TEST(BehaviorFsmBuilderTest, FullFluentChain) {
    enum class OrderState { Pending, Validated, Paid, Shipped };
    struct OrderData {
        std::string id;
        int amount = 0;
    };
    using OrderDirective = FsmDirective<OrderState, OrderData>;

    const TypeTag kValidateTag = static_cast<TypeTag>(1);
    const TypeTag kPayTag = static_cast<TypeTag>(2);
    const TypeTag kShipTag = static_cast<TypeTag>(3);

    std::vector<std::pair<OrderState, OrderState>> transition_log;

    auto bh =
        BehaviorFsmBuilder<OrderState, OrderData>::start(
            OrderState::Pending,
            OrderData{"order-1", 100}) // amount > 0 so validates
            .in_state(OrderState::Pending)
            .on_raw(kValidateTag,
                    [](TypedMessage& /*msg*/, OrderData& data) -> OrderDirective {
                        if (data.amount > 0)
                            return OrderDirective::go_to(OrderState::Validated);
                        return OrderDirective::stay();
                    })
            .in_state(OrderState::Validated)
            .on_raw(kPayTag,
                    [](TypedMessage& /*msg*/, OrderData& /*data*/) -> OrderDirective {
                        return OrderDirective::go_to(OrderState::Paid);
                    })
            .in_state(OrderState::Paid)
            .on_raw(kShipTag,
                    [](TypedMessage& /*msg*/, OrderData& /*data*/) -> OrderDirective {
                        return OrderDirective::go_to(OrderState::Shipped);
                    })
            .on_transition([&](OrderState from, OrderState to, OrderData& /*data*/) {
                transition_log.emplace_back(from, to);
            })
            .build(nullptr);

    auto msg1 = make_test_msg(kValidateTag);
    bh(msg1);
    auto msg2 = make_test_msg(kPayTag);
    bh(msg2);
    auto msg3 = make_test_msg(kShipTag);
    bh(msg3);

    ASSERT_EQ(transition_log.size(), 3);
    EXPECT_EQ(transition_log[0],
              (std::pair{OrderState::Pending, OrderState::Validated}));
    EXPECT_EQ(transition_log[1],
              (std::pair{OrderState::Validated, OrderState::Paid}));
    EXPECT_EQ(transition_log[2],
              (std::pair{OrderState::Paid, OrderState::Shipped}));
}

// ═════════════════════════════════════════════════════════════
// Integration tests
// ═════════════════════════════════════════════════════════════

TEST(BehaviorFsmIntegrationTest, MessageForWrongStateIsDropped) {
    enum class State { A, B };
    using SDirective = FsmDirective<State, int>;

    const TypeTag kAtoB = static_cast<TypeTag>(3001);
    const TypeTag kBHandler = static_cast<TypeTag>(3002);
    bool b_handler_called = false;

    auto bh = BehaviorFsmBuilder<State, int>::start(State::A, 0)
                  .in_state(State::A)
                  .on_raw(kAtoB,
                          [](TypedMessage& /*msg*/, int& /*data*/) -> SDirective {
                              return SDirective::go_to(State::B);
                          })
                  .in_state(State::B)
                  .on_raw(kBHandler,
                          [&](TypedMessage& /*msg*/, int& /*data*/) -> SDirective {
                              b_handler_called = true;
                              return SDirective::stay();
                          })
                  .build(nullptr);

    auto msg = make_test_msg(kBHandler);
    bh(msg);
    EXPECT_FALSE(b_handler_called);
}

TEST(BehaviorFsmIntegrationTest, StayWithDataUpdateVisibleToNextHandler) {
    enum class State { A };
    using SDirective = FsmDirective<State, int>;

    const TypeTag kIncrement = static_cast<TypeTag>(4001);
    const TypeTag kRead = static_cast<TypeTag>(4002);
    int read_value = -1;

    auto bh = BehaviorFsmBuilder<State, int>::start(State::A, 0)
                  .in_state(State::A)
                  .on_raw(kIncrement,
                          [](TypedMessage& /*msg*/, int& data) -> SDirective {
                              return SDirective::stay(data + 1);
                          })
                  .on_raw(kRead,
                          [&](TypedMessage& /*msg*/, int& data) -> SDirective {
                              read_value = data;
                              return SDirective::stay();
                          })
                  .build(nullptr);

    auto inc_msg = make_test_msg(kIncrement);
    bh(inc_msg);
    bh(inc_msg);

    auto read_msg = make_test_msg(kRead);
    bh(read_msg);
    EXPECT_EQ(read_value, 2);
}

TEST(BehaviorFsmIntegrationTest, StopPreventsFurtherDispatch) {
    enum class State { A };
    using SDirective = FsmDirective<State, int>;

    const TypeTag kStopTag = static_cast<TypeTag>(5001);
    const TypeTag kOtherTag = static_cast<TypeTag>(5002);
    bool handler_called_after_stop = false;

    // Register a handler after build to test stop behavior.
    // Use FsmRuntime directly for precise control.
    auto rt = std::make_shared<FsmRuntime<State, int>>(State::A, 0);

    rt->handlers[{State::A, kStopTag}] = [](TypedMessage& /*msg*/,
                                            int& /*data*/) -> SDirective {
        return SDirective::stop();
    };
    rt->handlers[{State::A, kOtherTag}] = [&](TypedMessage& /*msg*/,
                                              int& /*data*/) -> SDirective {
        handler_called_after_stop = true;
        return SDirective::stay();
    };

    // Stop the FSM
    auto stop_msg = make_test_msg(kStopTag);
    rt->dispatch(stop_msg);
    EXPECT_TRUE(rt->stopped);

    // Try another message
    auto other_msg = make_test_msg(kOtherTag);
    rt->dispatch(other_msg);
    EXPECT_FALSE(handler_called_after_stop);
}

TEST(BehaviorFsmIntegrationTest, OnTransitionCalledFromStateBuilder) {
    // Verify on_transition() works when called on StateBuilder
    // (delegates to parent BehaviorFsmBuilder).
    enum class State { A, B };
    using SDirective = FsmDirective<State, int>;

    const TypeTag kGoTag = static_cast<TypeTag>(6001);
    std::vector<std::pair<State, State>> transitions;

    auto bh = BehaviorFsmBuilder<State, int>::start(State::A, 0)
                  .in_state(State::A)
                  .on_raw(kGoTag,
                          [](TypedMessage& /*msg*/, int& /*data*/) -> SDirective {
                              return SDirective::go_to(State::B);
                          })
                  .on_transition([&](State from, State to, int& /*data*/) {
                      transitions.emplace_back(from, to);
                  })
                  .build(nullptr);

    auto msg = make_test_msg(kGoTag);
    bh(msg);

    ASSERT_EQ(transitions.size(), 1);
    EXPECT_EQ(transitions[0].first, State::A);
    EXPECT_EQ(transitions[0].second, State::B);
}

} // namespace
} // namespace hpactor
