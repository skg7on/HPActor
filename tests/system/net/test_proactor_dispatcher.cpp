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

#include <hpactor/net/proactor_dispatcher.hpp>

#include <optional>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

class ProactorDispatcherTest : public ::testing::Test {
  protected:
    ProactorDispatcher disp;
};

TEST_F(ProactorDispatcherTest, TimerFiredCallsTimerHandler) {
    std::optional<uint64_t> captured_handle;
    disp.set_timer_handler(
        [&captured_handle](uint64_t handle) { captured_handle = handle; });

    OpCompletion completion;
    completion.actor = ActorId(0);
    completion.type = OpType::TimerFired;
    completion.fd = -1;
    completion.result = 0;
    completion.user_data = 42;

    disp.on_completion(completion);

    ASSERT_TRUE(captured_handle.has_value());
    if (!captured_handle)
        GTEST_SKIP();
    EXPECT_EQ(captured_handle.value(), 42u);
}

TEST_F(ProactorDispatcherTest, SendRoutesViaCompletionCallback) {
    std::optional<OpCompletion> captured;
    disp.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    OpCompletion completion;
    completion.actor = ActorId(1);
    completion.type = OpType::Send;
    completion.fd = 10;
    completion.result = 5;

    disp.on_completion(completion);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::Send);
    EXPECT_EQ(captured->actor, ActorId(1));
    EXPECT_EQ(captured->result, 5);
}

TEST_F(ProactorDispatcherTest, RecvRoutesViaCompletionCallback) {
    std::optional<OpCompletion> captured;
    disp.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    OpCompletion completion;
    completion.actor = ActorId(2);
    completion.type = OpType::Recv;
    completion.fd = 20;
    completion.result = 100;

    disp.on_completion(completion);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::Recv);
    EXPECT_EQ(captured->actor, ActorId(2));
}

TEST_F(ProactorDispatcherTest, AcceptRoutesViaCompletionCallback) {
    std::optional<OpCompletion> captured;
    disp.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    OpCompletion completion;
    completion.actor = ActorId(3);
    completion.type = OpType::Accept;
    completion.fd = 7;
    completion.result = 8;

    disp.on_completion(completion);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::Accept);
    EXPECT_EQ(captured->result, 8);
}

TEST_F(ProactorDispatcherTest, ConnectRoutesViaCompletionCallback) {
    std::optional<OpCompletion> captured;
    disp.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    OpCompletion completion;
    completion.actor = ActorId(4);
    completion.type = OpType::Connect;
    completion.result = 0;

    disp.on_completion(completion);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::Connect);
    EXPECT_EQ(captured->result, 0);
}

TEST_F(ProactorDispatcherTest, RecvFromRoutesViaCompletionCallback) {
    std::optional<OpCompletion> captured;
    disp.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    OpCompletion completion;
    completion.actor = ActorId(5);
    completion.type = OpType::RecvFrom;
    completion.result = 64;

    disp.on_completion(completion);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::RecvFrom);
}

TEST_F(ProactorDispatcherTest, SendToRoutesViaCompletionCallback) {
    std::optional<OpCompletion> captured;
    disp.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    OpCompletion completion;
    completion.actor = ActorId(6);
    completion.type = OpType::SendTo;
    completion.result = 32;

    disp.on_completion(completion);

    ASSERT_TRUE(captured.has_value());
    if (!captured)
        GTEST_SKIP();
    EXPECT_EQ(captured->type, OpType::SendTo);
}

TEST_F(ProactorDispatcherTest, RegisterIoUnregisterIoracking) {
    disp.register_io(10, ActorId(1), OpType::Recv);
    EXPECT_TRUE(disp.has_active_io(10));
    disp.unregister_io(10);
    EXPECT_FALSE(disp.has_active_io(10));
}

TEST_F(ProactorDispatcherTest, NoCrashWithoutHandlers) {
    OpCompletion timer_comp;
    timer_comp.type = OpType::TimerFired;
    timer_comp.user_data = 1;
    disp.on_completion(timer_comp);

    OpCompletion io_comp;
    io_comp.type = OpType::Send;
    io_comp.actor = ActorId(1);
    io_comp.fd = 5;
    io_comp.result = 0;
    disp.on_completion(io_comp);

    // No crash = pass
}

TEST_F(ProactorDispatcherTest, TimerFiredDoesNotRouteToCompletionCallback) {
    bool timer_called = false;
    disp.set_timer_handler([&timer_called](uint64_t) { timer_called = true; });
    bool io_callback_called = false;
    disp.set_completion_callback(
        [&io_callback_called](OpCompletion) { io_callback_called = true; });

    OpCompletion completion;
    completion.type = OpType::TimerFired;
    completion.user_data = 99;

    disp.on_completion(completion);

    EXPECT_TRUE(timer_called);
    EXPECT_FALSE(io_callback_called);
}

TEST_F(ProactorDispatcherTest, CompletionAutoUnregistersActiveIo) {
    std::optional<OpCompletion> captured;
    disp.set_completion_callback([&captured](OpCompletion c) { captured = c; });

    disp.register_io(30, ActorId(7), OpType::Recv);
    ASSERT_TRUE(disp.has_active_io(30));

    OpCompletion completion;
    completion.type = OpType::Recv;
    completion.actor = ActorId(7);
    completion.fd = 30;
    completion.result = 10;

    disp.on_completion(completion);

    EXPECT_FALSE(disp.has_active_io(30));
}
