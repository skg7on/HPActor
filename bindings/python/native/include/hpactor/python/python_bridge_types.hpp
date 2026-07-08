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

#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/msg/message_id.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <memory>

namespace hpactor::python {

/// \brief Kinds of commands that the Python interpreter thread can send to the
///        native bridge actor.
enum class PythonCommandKind : uint8_t {
    Send,           ///< Send a message to a native actor.
    Reply,          ///< Reply to a request.
    ReplyError,     ///< Reply with an error code.
    Ask,            ///< Request-response ask.
    Spawn,          ///< Spawn a new Python actor.
    Schedule,       ///< Schedule a delayed message delivery.
    CancelSchedule, ///< Cancel a scheduled delivery.
    Link,           ///< Link two actors.
    Unlink,         ///< Unlink two actors.
    Monitor,        ///< Monitor an actor for termination.
    Demonitor,      ///< Stop monitoring an actor.
    Stop,           ///< Stop a Python actor.
    Passivate,      ///< Passivate a Python actor.
    ActorFailed,    ///< Report a Python actor failure.
    Inspect,        ///< Introspect a Python actor's state.
};

/// \brief Kinds of completions that the native bridge actor sends back to the
///        Python interpreter thread.
enum class PythonCompletionKind : uint8_t {
    CommandResult,  ///< Generic command completion.
    AskResult,      ///< Result of an ask request.
    DeliveryResult, ///< Delivery status report.
    SpawnResult,    ///< Result of a spawn request.
    InspectResult,  ///< Result of an inspect request.
};

/// \brief Envelope dispatched to a Python-bound actor for handling.
struct PythonDispatchEnvelope final {
    ActorAddress actor;
    uint64_t generation{0};
    TypeTag type_tag{TypeTag::Invalid};
    StreamBuffer payload;
    ActorAddress sender;
    MessageId message_id{};
    uint64_t ask_message_id{0};
    TraceContext trace{};
    bool has_trace{false};
    uint8_t priority{0};
    int64_t deadline_ns{INT64_MAX};
    uint32_t flags{0};
    bool ack_requested{false};
    uint64_t sequence{0};
};

/// \brief Command sent from the Python interpreter thread to the native bridge
///        actor for execution.
struct PythonCommand final {
    PythonCommandKind kind{PythonCommandKind::Send};
    ActorAddress origin;
    uint64_t generation{0};
    ActorAddress target;
    TypeTag type_tag{TypeTag::Invalid};
    StreamBuffer payload;
    uint64_t token{0};
    uint64_t sequence{0};
    uint8_t priority{0};
    int64_t deadline_ns{INT64_MAX};
    uint32_t flags{0};
    uint64_t delay_ns{0};
    uint32_t error_code{0};
};

/// \brief Completion sent from the native bridge actor back to the Python
///        interpreter thread reporting the result of a command.
struct PythonCompletion final {
    PythonCompletionKind kind{PythonCompletionKind::CommandResult};
    uint64_t token{0};
    uint64_t sequence{0};
    FailureReason failure{FailureReason::Unknown};
    ActorAddress actor;
    uint64_t generation{0};
    TypeTag type_tag{TypeTag::Invalid};
    StreamBuffer payload;
};

/// \brief Shared pointer aliases for zero-copy queue transfer.
using PythonDispatchPtr = std::shared_ptr<const PythonDispatchEnvelope>;
using PythonCommandPtr = std::shared_ptr<const PythonCommand>;
using PythonCompletionPtr = std::shared_ptr<const PythonCompletion>;

} // namespace hpactor::python
