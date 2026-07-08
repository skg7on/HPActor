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

#include <hpactor/python/python_gateway_actor.hpp>

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <memory>

namespace hpactor::python {

PythonGatewayActor::PythonGatewayActor(ActorContext* context, ActorSystem& system,
                                       PythonRuntime& runtime,
                                       PythonCommandExecutorPort executor) noexcept
    : EventBasedActor(context, system), runtime_(runtime), executor_(executor) {
    become(make_behavior());
}

void PythonGatewayActor::receive(TypedMessage& message) {
    // Intercept kPythonWakeupTag before the base system-tag switch.
    // This tag is in the protected subsystem range (0x80–0xFF) and is
    // deliberately handled here to avoid being routed through
    // EventBasedActor::dispatch_system_message().
    if (message.type_id() == kPythonWakeupTag) {
        handle_wakeup(message);
        return;
    }

    // Delegate all other messages to EventBasedActor::receive().
    EventBasedActor::receive(message);
}

void PythonGatewayActor::handle_wakeup(TypedMessage& message) noexcept {
    (void)message; // The wake message payload is empty; it is just a signal.

    size_t max_per_turn = runtime_.config().max_commands_per_turn;

    // Drain at most max_commands_per_turn commands, executing each through
    // the fixed executor port.
    (void)runtime_.drain_commands(max_per_turn, [this](const PythonCommand& cmd) {
        if (executor_) {
            auto exec_result = executor_.execute(executor_.context, cmd);
            if (exec_result.emit_completion) {
                auto completion =
                    std::make_shared<PythonCompletion>(exec_result.completion);
                (void)runtime_.try_push_completion(completion);
            }
        }
    });

    // If commands remain after draining the budget, self-send an empty
    // wake message to requeue for another scheduler turn.
    if (runtime_.snapshot().queues.command_depth > 0) {
        auto* ctx = context();
        if (ctx != nullptr) {
            TypedMessage wake_msg(kPythonWakeupTag, StreamBuffer{});
            ctx->send(address(), std::move(wake_msg));
        }
    }
}

} // namespace hpactor::python
