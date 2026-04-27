// Copyright 2026 HPActor Contributors
#include <hpactor/net/proactor_dispatcher.hpp>

#include <cassert>
#include <cstdio>
#include <optional>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    printf("=== ProactorDispatcher Tests ===\n");

    // Test 1: TimerFired calls timer handler
    {
        printf("Test 1: TimerFired route to timer handler... ");
        ProactorDispatcher disp;
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

        assert(captured_handle.has_value() &&
               "timer handler should be called");
        assert(captured_handle.value() == 42 && "user_data should be 42");
        printf("PASS\n");
    }

    // Test 2: Send completion routes via completion callback
    {
        printf("Test 2: Send route via completion callback... ");
        ProactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_callback(
            [&captured](OpCompletion c) { captured = c; });

        OpCompletion completion;
        completion.actor = ActorId(1);
        completion.type = OpType::Send;
        completion.fd = 10;
        completion.result = 5;

        disp.on_completion(completion);

        assert(captured.has_value() && "completion should be captured");
        assert(captured->type == OpType::Send);
        assert(captured->actor == ActorId(1));
        assert(captured->result == 5);
        printf("PASS\n");
    }

    // Test 3: Recv completion routes via completion callback
    {
        printf("Test 3: Recv route via completion callback... ");
        ProactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_callback(
            [&captured](OpCompletion c) { captured = c; });

        OpCompletion completion;
        completion.actor = ActorId(2);
        completion.type = OpType::Recv;
        completion.fd = 20;
        completion.result = 100;

        disp.on_completion(completion);

        assert(captured.has_value());
        assert(captured->type == OpType::Recv);
        assert(captured->actor == ActorId(2));
        printf("PASS\n");
    }

    // Test 4: Accept completion routes via completion callback
    {
        printf("Test 4: Accept route via completion callback... ");
        ProactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_callback(
            [&captured](OpCompletion c) { captured = c; });

        OpCompletion completion;
        completion.actor = ActorId(3);
        completion.type = OpType::Accept;
        completion.fd = 7;
        completion.result = 8;

        disp.on_completion(completion);

        assert(captured.has_value());
        assert(captured->type == OpType::Accept);
        assert(captured->result == 8);
        printf("PASS\n");
    }

    // Test 5: Connect completion routes via completion callback
    {
        printf("Test 5: Connect route via completion callback... ");
        ProactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_callback(
            [&captured](OpCompletion c) { captured = c; });

        OpCompletion completion;
        completion.actor = ActorId(4);
        completion.type = OpType::Connect;
        completion.result = 0;

        disp.on_completion(completion);

        assert(captured.has_value());
        assert(captured->type == OpType::Connect);
        assert(captured->result == 0);
        printf("PASS\n");
    }

    // Test 6: RecvFrom completion routes via completion callback
    {
        printf("Test 6: RecvFrom route via completion callback... ");
        ProactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_callback(
            [&captured](OpCompletion c) { captured = c; });

        OpCompletion completion;
        completion.actor = ActorId(5);
        completion.type = OpType::RecvFrom;
        completion.result = 64;

        disp.on_completion(completion);

        assert(captured.has_value());
        assert(captured->type == OpType::RecvFrom);
        printf("PASS\n");
    }

    // Test 7: SendTo completion routes via completion callback
    {
        printf("Test 7: SendTo route via completion callback... ");
        ProactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_callback(
            [&captured](OpCompletion c) { captured = c; });

        OpCompletion completion;
        completion.actor = ActorId(6);
        completion.type = OpType::SendTo;
        completion.result = 32;

        disp.on_completion(completion);

        assert(captured.has_value());
        assert(captured->type == OpType::SendTo);
        printf("PASS\n");
    }

    // Test 8: register_io / unregister_io tracking
    {
        printf("Test 8: register_io/unregister_io tracking... ");
        ProactorDispatcher disp;
        disp.register_io(10, ActorId(1), OpType::Recv);
        assert(disp.has_active_io(10) && "should track active io");
        disp.unregister_io(10);
        assert(!disp.has_active_io(10) && "should remove tracking");
        printf("PASS\n");
    }

    // Test 9: No crash without handlers set
    {
        printf("Test 9: no crash without handlers... ");
        ProactorDispatcher disp;

        OpCompletion timer_comp;
        timer_comp.type = OpType::TimerFired;
        timer_comp.user_data = 1;
        disp.on_completion(timer_comp); // no timer handler - should be no-op

        OpCompletion io_comp;
        io_comp.type = OpType::Send;
        io_comp.actor = ActorId(1);
        io_comp.fd = 5;
        io_comp.result = 0;
        disp.on_completion(io_comp); // no callback/system - should be no-op

        printf("PASS\n");
    }

    // Test 10: TimerFired does not call completion callback
    {
        printf("Test 10: TimerFired does not route to completion callback... ");
        ProactorDispatcher disp;
        bool timer_called = false;
        disp.set_timer_handler(
            [&timer_called](uint64_t) { timer_called = true; });
        bool io_callback_called = false;
        disp.set_completion_callback(
            [&io_callback_called](OpCompletion) { io_callback_called = true; });

        OpCompletion completion;
        completion.type = OpType::TimerFired;
        completion.user_data = 99;

        disp.on_completion(completion);

        assert(timer_called && "timer handler should be called");
        assert(!io_callback_called &&
               "completion callback should not receive TimerFired");
        printf("PASS\n");
    }

    // Test 11: Completion auto-unregisters active io
    {
        printf("Test 11: completion auto-unregisters active io... ");
        ProactorDispatcher disp;
        std::optional<OpCompletion> captured;
        disp.set_completion_callback(
            [&captured](OpCompletion c) { captured = c; });

        disp.register_io(30, ActorId(7), OpType::Recv);
        assert(disp.has_active_io(30));

        OpCompletion completion;
        completion.type = OpType::Recv;
        completion.actor = ActorId(7);
        completion.fd = 30;
        completion.result = 10;

        disp.on_completion(completion);

        assert(!disp.has_active_io(30) &&
               "fd should be auto-unregistered after completion");
        printf("PASS\n");
    }

    printf("=== All ProactorDispatcher Tests Passed ===\n");
    return 0;
}
