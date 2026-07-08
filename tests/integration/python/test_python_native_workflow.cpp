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

#include <scheduler_test_driver.hpp>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/python/python_bridge_actor.hpp>
#include <hpactor/python/python_gateway_actor.hpp>
#include <hpactor/python/python_gateway_wake_adapter.hpp>
#include <hpactor/python/python_runtime.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <vector>

using namespace hpactor;

namespace {
struct WorkflowExecutor {
    std::vector<uint64_t> commands;

    static python::PythonCommandExecution
    execute(void* context, const python::PythonCommand& command) noexcept {
        auto* self = static_cast<WorkflowExecutor*>(context);
        self->commands.push_back(command.sequence);
        python::PythonCompletion completion;
        completion.kind = python::PythonCompletionKind::CommandResult;
        completion.token = command.token;
        completion.sequence = command.sequence;
        completion.actor = command.origin;
        completion.generation = command.generation;
        return {true, std::move(completion)};
    }
};
} // namespace

TEST(PythonNativeWorkflowTest, PreservesOrderIdentityAndDrainAdmission) {
    // Create runtime first so it outlives the actor system.
    // The PythonBridgeActor holds a reference to the runtime, and its
    // lease destructor accesses the runtime during ActorSystem teardown.
    python::PythonRuntimeConfig runtime_cfg;
    runtime_cfg.max_commands_per_turn = 2;
    auto created = python::PythonRuntime::create(runtime_cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());

    WorkflowExecutor executor;

    Config cfg;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    auto gateway = system.spawn<python::PythonGatewayActor>(
        *runtime,
        python::PythonCommandExecutorPort{&executor, &WorkflowExecutor::execute});
    python::PythonGatewayWakeAdapter wake(system, gateway.address());
    ASSERT_TRUE(runtime->start(wake.port()).ok());

    auto lease = runtime->reserve_actor();
    ASSERT_TRUE(lease.has_value());
    auto bridge =
        system.spawn<python::PythonBridgeActor>(*runtime, std::move(*lease));
    auto* bridge_actor =
        static_cast<python::PythonBridgeActor*>(bridge.get().get());
    ASSERT_NE(bridge_actor, nullptr);

    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        TypedMessage message(TypeTag::User,
                             StreamBuffer{static_cast<uint8_t>(sequence)});
        mailbox::DeliveryOptions options;
        options.message_id = 100 + sequence;
        options.flags = static_cast<uint32_t>(sequence);
        ASSERT_TRUE(system
                        .try_deliver_local(bridge.id(), std::move(message),
                                           static_cast<uint8_t>(sequence - 1),
                                           INT64_MAX, options)
                        .accepted());
    }
    ASSERT_TRUE(driver.drain_until(
        [&] { return runtime->snapshot().queues.dispatch_depth == 3; }));

    std::vector<uint64_t> dispatch_sequences;
    ASSERT_EQ(runtime->drain_dispatch(
                  3,
                  [&](const auto& envelope) {
                      EXPECT_EQ(envelope.actor.id, bridge.id());
                      EXPECT_EQ(envelope.generation, bridge_actor->generation());
                      dispatch_sequences.push_back(envelope.sequence);

                      auto command = std::make_shared<python::PythonCommand>();
                      command->kind = python::PythonCommandKind::Send;
                      command->origin = envelope.actor;
                      command->generation = envelope.generation;
                      command->target = gateway.address();
                      command->token = 200 + envelope.sequence;
                      command->sequence = envelope.sequence;
                      ASSERT_TRUE(runtime->try_push_command(command));
                  }),
              3u);
    EXPECT_EQ(dispatch_sequences, (std::vector<uint64_t>{1, 2, 3}));

    ASSERT_TRUE(driver.drain_until(
        [&] { return runtime->snapshot().queues.completion_depth == 3; }));
    EXPECT_EQ(executor.commands, (std::vector<uint64_t>{1, 2, 3}));

    std::vector<uint64_t> completion_sequences;
    ASSERT_EQ(runtime->drain_completions(3,
                                         [&](const auto& completion) {
                                             completion_sequences.push_back(
                                                 completion.sequence);
                                         }),
              3u);
    EXPECT_EQ(completion_sequences, (std::vector<uint64_t>{1, 2, 3}));

    runtime->begin_draining();
    EXPECT_FALSE(runtime->try_push_dispatch(
        std::make_shared<python::PythonDispatchEnvelope>()));
    EXPECT_FALSE(
        runtime->try_push_command(std::make_shared<python::PythonCommand>()));
    ASSERT_TRUE(runtime->stop().ok());
    EXPECT_EQ(runtime->snapshot().state, python::PythonRuntimeState::Stopped);
}
