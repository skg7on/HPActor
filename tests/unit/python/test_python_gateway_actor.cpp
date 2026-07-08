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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/python/python_gateway_actor.hpp>
#include <hpactor/python/python_gateway_wake_adapter.hpp>
#include <hpactor/python/python_runtime.hpp>

#include <vector>

using namespace hpactor;

namespace {
struct CommandProbe {
    std::vector<uint64_t> sequences;
    static python::PythonCommandExecution
    execute(void* context, const python::PythonCommand& command) noexcept {
        auto* self = static_cast<CommandProbe*>(context);
        self->sequences.push_back(command.sequence);
        python::PythonCompletion completion;
        completion.token = command.token;
        completion.sequence = command.sequence;
        return {true, std::move(completion)};
    }
};
} // namespace

TEST(PythonGatewayActorTest, SpawnAndStopGateway) {
    // Create runtime first so it outlives the actor system.
    python::PythonRuntimeConfig runtime_cfg;
    runtime_cfg.max_commands_per_turn = 2;
    auto created = python::PythonRuntime::create(runtime_cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());

    Config cfg;
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    CommandProbe commands;
    auto gateway = system.spawn<python::PythonGatewayActor>(
        *runtime,
        python::PythonCommandExecutorPort{&commands, &CommandProbe::execute});
    ASSERT_NE(gateway.id().value(), 0u);
    python::PythonGatewayWakeAdapter wake(system, gateway.address());
    ASSERT_TRUE(runtime->start(wake.port()).ok());
    EXPECT_TRUE(runtime->stop().ok());
}

TEST(PythonGatewayActorTest, DrainBudgetRequeuesRemainingCommands) {
    // Create runtime first so it outlives the actor system. The gateway holds
    // a reference to the runtime, and the SchedulerTestDriver destructor
    // resumes workers which may process stale wake messages in the gateway's
    // mailbox.
    python::PythonRuntimeConfig runtime_cfg;
    runtime_cfg.max_commands_per_turn = 2;
    auto created = python::PythonRuntime::create(runtime_cfg);
    ASSERT_TRUE(created.ok());
    auto runtime = std::move(created.value());

    Config cfg;
    cfg.scheduler_start_paused = true;
    ActorSystem system(cfg);
    test::SchedulerTestDriver driver(system);

    CommandProbe commands;
    auto gateway = system.spawn<python::PythonGatewayActor>(
        *runtime,
        python::PythonCommandExecutorPort{&commands, &CommandProbe::execute});
    python::PythonGatewayWakeAdapter wake(system, gateway.address());
    ASSERT_TRUE(runtime->start(wake.port()).ok());

    for (uint64_t sequence = 1; sequence <= 3; ++sequence) {
        auto command = std::make_shared<python::PythonCommand>();
        command->sequence = sequence;
        command->token = sequence + 100;
        ASSERT_TRUE(runtime->try_push_command(command));
    }

    // Drain until the gateway has processed the first two commands (budget=2)
    // and one command remains in the queue.
    ASSERT_TRUE(driver.drain_until([&] {
        return commands.sequences.size() == 2 &&
               runtime->snapshot().queues.command_depth == 1;
    }));
    EXPECT_EQ(commands.sequences, (std::vector<uint64_t>{1, 2}));
    EXPECT_EQ(runtime->snapshot().queues.command_depth, 1u);

    // Drain until the gateway has processed the remaining command.
    ASSERT_TRUE(driver.drain_until([&] {
        return commands.sequences.size() == 3 &&
               runtime->snapshot().queues.command_depth == 0;
    }));
    EXPECT_EQ(commands.sequences, (std::vector<uint64_t>{1, 2, 3}));
    EXPECT_EQ(runtime->snapshot().queues.completion_depth, 3u);
    EXPECT_TRUE(runtime->stop().ok());
}
