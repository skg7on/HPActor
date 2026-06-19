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
#include <hpactor/msg/typed_message.hpp>

#include <string>
#include <vector>

namespace hpactor {
namespace {

// ── Helper: construct a simple TypedMessage for testing ──────────

TypedMessage make_test_msg(TypeTag tag = static_cast<TypeTag>(1)) {
    StreamBuffer payload;
    payload.resize(4, 0);
    return TypedMessage(tag, std::move(payload));
}

// ═════════════════════════════════════════════════════════════════
// Behavior::receive — explicit named factory
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorReceiveTest, InvokesHandler) {
    bool called = false;
    auto bh = Behavior::receive([&](TypedMessage& /*msg*/) { called = true; });
    EXPECT_TRUE(bh);
    auto msg = make_test_msg();
    bh(msg);
    EXPECT_TRUE(called);
}

TEST(BehaviorReceiveTest, ReceivesMessageContent) {
    int received_tag = 0;
    auto bh = Behavior::receive([&](TypedMessage& msg) {
        received_tag = static_cast<int>(msg.type_id());
    });
    auto msg = make_test_msg(static_cast<TypeTag>(42));
    bh(msg);
    EXPECT_EQ(received_tag, 42);
}

// ═════════════════════════════════════════════════════════════════
// Behavior::empty — explicit no-op behavior
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorEmptyTest, EvaluatesToFalse) {
    auto bh = Behavior::empty();
    EXPECT_FALSE(bh);
}

TEST(BehaviorEmptyTest, DoesNotCrashOnInvoke) {
    auto bh = Behavior::empty();
    auto msg = make_test_msg();
    bh(msg); // must not crash
}

// ═════════════════════════════════════════════════════════════════
// Behavior::setup — deferred initialization combinator
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorSetupTest, DefersInitializationUntilFirstMessage) {
    bool factory_called = false;

    auto bh = Behavior::setup([&]() -> Behavior {
        factory_called = true;
        return Behavior::receive([](TypedMessage& /*msg*/) {});
    });

    // Factory must NOT be called during setup()
    EXPECT_FALSE(factory_called);
    EXPECT_TRUE(bh); // setup behavior is non-empty

    // First invoke triggers the factory
    auto msg = make_test_msg();
    bh(msg);
    EXPECT_TRUE(factory_called);
}

TEST(BehaviorSetupTest, DelegatesToProducedBehavior) {
    int call_count = 0;

    auto bh = Behavior::setup([&]() -> Behavior {
        return Behavior::receive([&](TypedMessage& /*msg*/) { call_count++; });
    });

    auto msg1 = make_test_msg(static_cast<TypeTag>(1));
    auto msg2 = make_test_msg(static_cast<TypeTag>(2));

    bh(msg1);
    EXPECT_EQ(call_count, 1);

    bh(msg2);
    EXPECT_EQ(call_count, 2); // second message also delegated
}

TEST(BehaviorSetupTest, FactoryCalledOnlyOnce) {
    int factory_calls = 0;
    int handler_calls = 0;

    auto bh = Behavior::setup([&]() -> Behavior {
        factory_calls++;
        return Behavior::receive([&](TypedMessage& /*msg*/) { handler_calls++; });
    });

    auto msg1 = make_test_msg(static_cast<TypeTag>(1));
    auto msg2 = make_test_msg(static_cast<TypeTag>(2));
    auto msg3 = make_test_msg(static_cast<TypeTag>(3));

    bh(msg1);
    bh(msg2);
    bh(msg3);

    EXPECT_EQ(factory_calls, 1);
    EXPECT_EQ(handler_calls, 3);
}

TEST(BehaviorSetupTest, SetupCanCaptureActorContext) {
    // Simulate an actor-like context with a shared state
    int context_value = 42;
    int captured_value = 0;

    // The setup factory captures the "context" by reference
    auto bh = Behavior::setup([&]() -> Behavior {
        captured_value = context_value; // "use" the context
        return Behavior::receive([&](TypedMessage& /*msg*/) {
            // handler
        });
    });

    auto msg = make_test_msg();
    bh(msg);
    EXPECT_EQ(captured_value, 42);
}

// ═════════════════════════════════════════════════════════════════
// Behavior::intercept — middleware/decorator combinator
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorInterceptTest, PreProcessingBeforeInner) {
    std::vector<std::string> order;

    auto inner = Behavior::receive(
        [&](TypedMessage& /*msg*/) { order.emplace_back("inner"); });

    auto bh = Behavior::intercept(std::move(inner),
                                  [&](TypedMessage& msg, Behavior::next_fn next) {
                                      order.emplace_back("before");
                                      next(msg);
                                      order.emplace_back("after");
                                  });

    auto msg = make_test_msg();
    bh(msg);

    ASSERT_EQ(order.size(), 3);
    EXPECT_EQ(order[0], "before");
    EXPECT_EQ(order[1], "inner");
    EXPECT_EQ(order[2], "after");
}

TEST(BehaviorInterceptTest, CanSuppressInnerCall) {
    bool inner_called = false;

    auto inner =
        Behavior::receive([&](TypedMessage& /*msg*/) { inner_called = true; });

    auto bh = Behavior::intercept(std::move(inner),
                                  [&](TypedMessage& msg, Behavior::next_fn next) {
                                      // Intentionally NOT calling next(msg) —
                                      // suppress
                                      (void)msg;
                                      (void)next;
                                  });

    auto msg = make_test_msg();
    bh(msg);
    EXPECT_FALSE(inner_called);
}

TEST(BehaviorInterceptTest, CanModifyMessageBeforeInner) {
    int received_tag = 0;

    auto inner = Behavior::receive([&](TypedMessage& msg) {
        received_tag = static_cast<int>(msg.type_id());
    });

    auto bh = Behavior::intercept(std::move(inner),
                                  [](TypedMessage& msg, Behavior::next_fn next) {
                                      // Observe and pass through
                                      next(msg);
                                  });

    auto msg = make_test_msg(static_cast<TypeTag>(99));
    bh(msg);
    EXPECT_EQ(received_tag, 99);
}

TEST(BehaviorInterceptTest, MultipleInterceptorLayers) {
    std::vector<std::string> order;

    auto inner = Behavior::receive(
        [&](TypedMessage& /*msg*/) { order.emplace_back("inner"); });

    auto layer1 = Behavior::intercept(
        std::move(inner), [&](TypedMessage& msg, Behavior::next_fn next) {
            order.emplace_back("l1-before");
            next(msg);
            order.emplace_back("l1-after");
        });

    auto layer2 = Behavior::intercept(
        std::move(layer1), [&](TypedMessage& msg, Behavior::next_fn next) {
            order.emplace_back("l2-before");
            next(msg);
            order.emplace_back("l2-after");
        });

    auto msg = make_test_msg();
    layer2(msg);

    ASSERT_EQ(order.size(), 5);
    EXPECT_EQ(order[0], "l2-before");
    EXPECT_EQ(order[1], "l1-before");
    EXPECT_EQ(order[2], "inner");
    EXPECT_EQ(order[3], "l1-after");
    EXPECT_EQ(order[4], "l2-after");
}

// ═════════════════════════════════════════════════════════════════
// Behavior::compose — chain two behaviors (first, then second)
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorComposeTest, BothBehaviorsCalled) {
    bool first_called = false;
    bool second_called = false;

    auto first =
        Behavior::receive([&](TypedMessage& /*msg*/) { first_called = true; });
    auto second =
        Behavior::receive([&](TypedMessage& /*msg*/) { second_called = true; });

    auto bh = Behavior::compose(std::move(first), std::move(second));

    auto msg = make_test_msg();
    bh(msg);

    EXPECT_TRUE(first_called);
    EXPECT_TRUE(second_called);
}

TEST(BehaviorComposeTest, OrderIsFirstThenSecond) {
    std::vector<int> order;

    auto first =
        Behavior::receive([&](TypedMessage& /*msg*/) { order.emplace_back(1); });
    auto second =
        Behavior::receive([&](TypedMessage& /*msg*/) { order.emplace_back(2); });

    auto bh = Behavior::compose(std::move(first), std::move(second));

    auto msg = make_test_msg();
    bh(msg);

    ASSERT_EQ(order.size(), 2);
    EXPECT_EQ(order[0], 1);
    EXPECT_EQ(order[1], 2);
}

TEST(BehaviorComposeTest, EmptyFirstDoesNotBlockSecond) {
    bool second_called = false;

    auto first = Behavior::empty();
    auto second =
        Behavior::receive([&](TypedMessage& /*msg*/) { second_called = true; });

    auto bh = Behavior::compose(std::move(first), std::move(second));

    auto msg = make_test_msg();
    bh(msg);

    EXPECT_TRUE(second_called);
}

TEST(BehaviorComposeTest, ComposeIsAssociative) {
    int a_val = 0, b_val = 0, c_val = 0;

    auto a = Behavior::receive([&](TypedMessage& /*msg*/) { a_val = 1; });
    auto b = Behavior::receive([&](TypedMessage& /*msg*/) { b_val = 2; });
    auto c = Behavior::receive([&](TypedMessage& /*msg*/) { c_val = 3; });

    // (a >> b) >> c
    auto ab = Behavior::compose(std::move(a), std::move(b));
    auto abc = Behavior::compose(std::move(ab), std::move(c));

    auto msg = make_test_msg();
    abc(msg);

    EXPECT_EQ(a_val, 1);
    EXPECT_EQ(b_val, 2);
    EXPECT_EQ(c_val, 3);
}

// ═════════════════════════════════════════════════════════════════
// Behavior::on_signal — handle specific TypeTag signals
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorOnSignalTest, MatchingSignalConsumedByHandler) {
    bool signal_called = false;
    bool inner_called = false;

    auto inner =
        Behavior::receive([&](TypedMessage& /*msg*/) { inner_called = true; });

    const TypeTag kMySignal = static_cast<TypeTag>(0x100);
    auto bh = Behavior::on_signal(
        kMySignal, [&](TypedMessage& /*msg*/) { signal_called = true; },
        std::move(inner));

    // Send a matching signal
    auto signal_msg = make_test_msg(kMySignal);
    bh(signal_msg);
    EXPECT_TRUE(signal_called);
    EXPECT_FALSE(inner_called); // inner should NOT be called for signal

    // Send a non-matching message
    auto normal_msg = make_test_msg(static_cast<TypeTag>(99));
    bh(normal_msg);
    EXPECT_TRUE(inner_called); // inner should be called for non-signal
}

TEST(BehaviorOnSignalTest, NonMatchingTagPassesThrough) {
    bool inner_called = false;
    int received_tag = 0;

    auto inner = Behavior::receive([&](TypedMessage& msg) {
        inner_called = true;
        received_tag = static_cast<int>(msg.type_id());
    });

    auto bh = Behavior::on_signal(
        static_cast<TypeTag>(0x200),
        [](TypedMessage& /*msg*/) { /* never called */ }, std::move(inner));

    auto msg = make_test_msg(static_cast<TypeTag>(55));
    bh(msg);
    EXPECT_TRUE(inner_called);
    EXPECT_EQ(received_tag, 55);
}

TEST(BehaviorOnSignalTest, MultipleSignals) {
    std::string last_handler;

    auto inner =
        Behavior::receive([&](TypedMessage& /*msg*/) { last_handler = "inner"; });

    auto with_sig_a = Behavior::on_signal(
        static_cast<TypeTag>(0x100),
        [&](TypedMessage& /*msg*/) { last_handler = "sig_a"; }, std::move(inner));

    auto bh = Behavior::on_signal(
        static_cast<TypeTag>(0x200),
        [&](TypedMessage& /*msg*/) { last_handler = "sig_b"; },
        std::move(with_sig_a));

    // Signal A
    auto msg_a = make_test_msg(static_cast<TypeTag>(0x100));
    bh(msg_a);
    EXPECT_EQ(last_handler, "sig_a");

    // Signal B
    auto msg_b = make_test_msg(static_cast<TypeTag>(0x200));
    bh(msg_b);
    EXPECT_EQ(last_handler, "sig_b");

    // Normal message
    auto msg_n = make_test_msg(static_cast<TypeTag>(42));
    bh(msg_n);
    EXPECT_EQ(last_handler, "inner");
}

// ═════════════════════════════════════════════════════════════════
// Combined combinator usage — real-world patterns
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorCombinedTest, SetupWithIntercept) {
    // Pattern: setup() initializes state, intercept() adds cross-cutting
    // concern
    bool setup_run = false;
    bool interceptor_run = false;
    bool handler_run = false;

    auto bh = Behavior::setup([&]() -> Behavior {
        setup_run = true;
        auto inner =
            Behavior::receive([&](TypedMessage& /*msg*/) { handler_run = true; });
        return Behavior::intercept(std::move(inner),
                                   [&](TypedMessage& msg, Behavior::next_fn next) {
                                       interceptor_run = true;
                                       next(msg);
                                   });
    });

    auto msg = make_test_msg();
    bh(msg);

    EXPECT_TRUE(setup_run);
    EXPECT_TRUE(interceptor_run);
    EXPECT_TRUE(handler_run);
}

TEST(BehaviorCombinedTest, SetupWithCompose) {
    // Pattern: setup provides context, compose layers two handlers
    int first_val = 0;
    int second_val = 0;

    auto bh = Behavior::setup([&]() -> Behavior {
        auto a = Behavior::receive([&](TypedMessage& /*msg*/) { first_val = 10; });
        auto b =
            Behavior::receive([&](TypedMessage& /*msg*/) { second_val = 20; });
        return Behavior::compose(std::move(a), std::move(b));
    });

    auto msg = make_test_msg();
    bh(msg);

    EXPECT_EQ(first_val, 10);
    EXPECT_EQ(second_val, 20);
}

TEST(BehaviorCombinedTest, OnSignalWithInterceptComposition) {
    // Pattern: signal handler wrapping an intercepted behavior
    std::vector<std::string> trace;
    bool signal_caught = false;

    auto inner = Behavior::receive(
        [&](TypedMessage& /*msg*/) { trace.emplace_back("inner"); });

    auto intercepted = Behavior::intercept(
        std::move(inner), [&](TypedMessage& msg, Behavior::next_fn next) {
            trace.emplace_back("intercept-before");
            next(msg);
            trace.emplace_back("intercept-after");
        });

    auto bh = Behavior::on_signal(
        static_cast<TypeTag>(0xFF),
        [&](TypedMessage& /*msg*/) {
            signal_caught = true;
            trace.emplace_back("signal");
        },
        std::move(intercepted));

    // Normal message — goes through interceptor → inner
    auto normal = make_test_msg(static_cast<TypeTag>(1));
    bh(normal);
    EXPECT_FALSE(signal_caught);
    ASSERT_EQ(trace.size(), 3);
    EXPECT_EQ(trace[0], "intercept-before");
    EXPECT_EQ(trace[1], "inner");
    EXPECT_EQ(trace[2], "intercept-after");

    // Signal — consumed by signal handler, bypassing interceptor
    trace.clear();
    auto sig = make_test_msg(static_cast<TypeTag>(0xFF));
    bh(sig);
    EXPECT_TRUE(signal_caught);
    ASSERT_EQ(trace.size(), 1);
    EXPECT_EQ(trace[0], "signal");
}

// ═════════════════════════════════════════════════════════════════
// Edge cases
// ═════════════════════════════════════════════════════════════════

TEST(BehaviorEdgeCaseTest, DefaultConstructedIsEmpty) {
    Behavior bh;
    EXPECT_FALSE(bh);
}

TEST(BehaviorEdgeCaseTest, ReceiveFactoryProducesNonEmpty) {
    auto bh = Behavior::receive([](TypedMessage&) {});
    EXPECT_TRUE(bh);
}

TEST(BehaviorEdgeCaseTest, ComposeWithTwoEmptyIsEmpty) {
    auto bh = Behavior::compose(Behavior::empty(), Behavior::empty());
    EXPECT_FALSE(bh);
}

TEST(BehaviorEdgeCaseTest, ComposeWithOneNonEmptyIsNonEmpty) {
    auto bh = Behavior::compose(Behavior::empty(),
                                Behavior::receive([](TypedMessage&) {}));
    EXPECT_TRUE(bh);
}

TEST(BehaviorEdgeCaseTest, InterceptWithInterceptorIsNonEmpty) {
    // Even with empty inner, a non-null interceptor means the behavior
    // IS non-empty — the interceptor may take action on messages.
    auto bh = Behavior::intercept(Behavior::empty(),
                                  [](TypedMessage&, Behavior::next_fn) {});
    EXPECT_TRUE(bh); // interceptor is set, so behavior is active
}

TEST(BehaviorEdgeCaseTest, InterceptWithNullInterceptorAndEmptyIsEmpty) {
    auto bh = Behavior::intercept(
        Behavior::empty(), std::function<void(TypedMessage&, Behavior::next_fn)>{});
    EXPECT_FALSE(bh); // null interceptor + empty inner = empty
}

TEST(BehaviorEdgeCaseTest, OnSignalWithHandlerIsNonEmpty) {
    // Even with empty inner, a non-null signal handler means the
    // behavior IS non-empty — it handles matching signals.
    auto bh = Behavior::on_signal(
        static_cast<TypeTag>(0x100), [](TypedMessage&) {}, Behavior::empty());
    EXPECT_TRUE(bh); // handler is set, so behavior is active
}

} // namespace
} // namespace hpactor
