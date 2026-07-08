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

#include <hpactor/python/python_command_router.hpp>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/python/python_command_codec.hpp>
#include <hpactor/python/python_type_tags.hpp>

namespace hpactor::python {

PythonCommandRouter::PythonCommandRouter(ActorSystem& system,
                                         PythonRuntime& runtime) noexcept
    : system_(system), runtime_(runtime) {}

PythonCommandExecutorPort PythonCommandRouter::port() noexcept {
    return PythonCommandExecutorPort{this, &PythonCommandRouter::execute};
}

PythonCommandExecution
PythonCommandRouter::execute(void* context, const PythonCommand& command) noexcept {
    auto* self = static_cast<PythonCommandRouter*>(context);
    PythonCommandExecution result{};

    // Validate origin.
    if (command.origin.id == ActorId{}) {
        result.emit_completion = true;
        result.completion.kind = PythonCompletionKind::CommandResult;
        result.completion.token = command.token;
        result.completion.sequence = command.sequence;
        result.completion.failure = FailureReason::RejectedByPolicy;
        result.completion.source = FailureSource::LanguageBinding;
        result.completion.error_code =
            static_cast<uint32_t>(errors::invalid_argument);
        result.completion.detail = "invalid origin actor";
        return result;
    }

    // Validate generation.
    if (!self->runtime_.generation_matches(command.origin.id, command.generation)) {
        result.emit_completion = true;
        result.completion.kind = PythonCompletionKind::CommandResult;
        result.completion.token = command.token;
        result.completion.sequence = command.sequence;
        result.completion.failure = FailureReason::RejectedByPolicy;
        result.completion.source = FailureSource::LanguageBinding;
        result.completion.error_code =
            static_cast<uint32_t>(errors::actor_not_found);
        result.completion.detail = "stale generation";
        return result;
    }

    // Encode and deliver to the bridge actor.
    if (command.kind == PythonCommandKind::ActorFailed) {
        // Encode the PbPythonActorFailed and deliver on F2 tag.
        auto encoded = encode_actor_failed(
            command.origin, command.generation,
            command.detail.empty() ? "Exception" : command.detail,
            command.detail, std::to_string(command.error_code), command.sequence);
        if (!encoded.ok()) {
            result.emit_completion = true;
            result.completion.kind = PythonCompletionKind::ActorFailed;
            result.completion.token = command.token;
            result.completion.sequence = command.sequence;
            result.completion.failure = FailureReason::RejectedByPolicy;
            result.completion.source = FailureSource::LanguageBinding;
            return result;
        }
        TypedMessage msg(kPythonActorFailedTag, std::move(encoded.value()));
        msg.set_sender_address(command.origin);
        self->system_.deliver_with_result(command.origin.id, std::move(msg));
    } else {
        // Encode PbPythonActorCommand and deliver on F1 tag.
        auto encoded = encode_actor_command(command);
        if (!encoded.ok()) {
            result.emit_completion = true;
            result.completion.kind = PythonCompletionKind::CommandResult;
            result.completion.token = command.token;
            result.completion.sequence = command.sequence;
            result.completion.failure = FailureReason::RejectedByPolicy;
            result.completion.source = FailureSource::LanguageBinding;
            return result;
        }
        TypedMessage msg(kPythonActorCommandTag, std::move(encoded.value()));
        msg.set_sender_address(command.origin);
        self->system_.deliver_with_result(command.origin.id, std::move(msg));
    }

    return result;
}

} // namespace hpactor::python
