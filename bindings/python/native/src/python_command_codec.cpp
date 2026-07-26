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

#include <hpactor/python/python_command_codec.hpp>

#include <hpactor/msg/frame.hpp>
#include <hpactor/msg/type_tag.hpp>

#include <hpactor/python_binding_internal.pb.h>

namespace hpactor::python {

namespace {

constexpr uint32_t kCodecVersion = 1;
constexpr size_t kMaxPayloadBytes = 16 * 1024 * 1024; // 16 MiB
constexpr size_t kMaxDetailBytes = 16 * 1024;         // 16 KiB
constexpr size_t kMaxTracebackBytes = 16 * 1024;      // 16 KiB
constexpr size_t kMaxNameBytes = 255;
constexpr uint32_t kMinApplicationTag = 0x1000;
constexpr uint32_t kMaxApplicationTag = 0x00FFFFFF;

using internal::PythonCommandKind;

constexpr bool is_valid_application_tag(uint32_t tag) noexcept {
    return tag >= kMinApplicationTag && tag <= kMaxApplicationTag;
}

} // namespace

result<StreamBuffer> encode_actor_command(const PythonCommand& command) noexcept {
    internal::PbPythonActorCommand pb;

    pb.set_version(kCodecVersion);
    pb.set_kind(static_cast<internal::PythonCommandKind>(command.kind));
    pb.set_token(command.token);
    pb.set_sequence(command.sequence);
    pb.set_generation(command.generation);

    net::to_proto(pb.mutable_origin(), command.origin);
    net::to_proto(pb.mutable_target(), command.target);
    net::to_proto(pb.mutable_reply_to(), command.reply_to);

    pb.set_type_tag(static_cast<uint32_t>(command.type_tag));
    pb.set_payload(command.payload.data(), command.payload.size());
    pb.set_message_id(command.message_id);
    pb.set_ask_message_id(command.ask_message_id);
    pb.set_priority(command.priority);
    pb.set_deadline_ns(command.deadline_ns);
    pb.set_flags(command.flags);
    pb.set_delay_ns(command.delay_ns);
    pb.set_schedule_handle(command.schedule_handle);
    pb.set_error_code(command.error_code);

    if (!command.detail.empty()) {
        pb.set_detail(command.detail);
    }
    if (!command.actor_name.empty()) {
        pb.set_actor_name(command.actor_name);
    }
    pb.set_delivery_mode(command.delivery_mode);
    pb.set_no_drop(command.no_drop);
    pb.set_emit_backpressure(command.emit_backpressure);
    pb.set_retry_max_attempts(command.retry_max_attempts);
    pb.set_retry_per_attempt_timeout_ms(command.retry_per_attempt_timeout_ms);
    pb.set_retry_initial_backoff_ms(command.retry_initial_backoff_ms);
    pb.set_retry_max_backoff_ms(command.retry_max_backoff_ms);
    pb.set_retry_backoff(command.retry_backoff);
    pb.set_retry_jitter(command.retry_jitter);

    std::string serialized;
    if (!pb.SerializeToString(&serialized)) {
        return result<StreamBuffer>::make(
            error(errors::unknown, "failed to encode command"));
    }

    return result<StreamBuffer>::make(StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size()));
}

result<PythonCommand> decode_actor_command(const StreamBuffer& buffer) noexcept {
    internal::PbPythonActorCommand pb;
    if (!pb.ParseFromArray(buffer.data(), static_cast<int>(buffer.size()))) {
        return result<PythonCommand>::make(
            error(errors::unknown, "failed to parse command"));
    }

    if (pb.version() != kCodecVersion) {
        return result<PythonCommand>::make(
            error(errors::invalid_argument, "unsupported command version"));
    }

    // Validate bounded fields.
    if (pb.payload().size() > kMaxPayloadBytes) {
        return result<PythonCommand>::make(
            error(errors::invalid_argument, "command payload too large"));
    }
    if (pb.detail().size() > kMaxDetailBytes) {
        return result<PythonCommand>::make(
            error(errors::invalid_argument, "command detail too large"));
    }
    if (pb.actor_name().size() > kMaxNameBytes) {
        return result<PythonCommand>::make(
            error(errors::invalid_argument, "actor name too large"));
    }

    auto kind = pb.kind();
    if (kind < internal::PythonCommandKind::SEND ||
        kind > internal::PythonCommandKind_MAX) {
        return result<PythonCommand>::make(
            error(errors::invalid_argument, "invalid command kind"));
    }

    // Reject non-application type tags.
    if (!is_valid_application_tag(pb.type_tag())) {
        // Tag 0 is allowed for commands without a message type (e.g., Stop).
        if (pb.type_tag() != 0) {
            return result<PythonCommand>::make(error(
                errors::invalid_argument, "type tag outside application range"));
        }
    }

    PythonCommand cmd;
    cmd.kind = static_cast<hpactor::python::PythonCommandKind>(kind);
    cmd.token = pb.token();
    cmd.sequence = pb.sequence();
    cmd.generation = pb.generation();

    if (pb.has_origin()) {
        cmd.origin = net::from_proto(pb.origin());
    }
    if (pb.has_target()) {
        cmd.target = net::from_proto(pb.target());
    }
    if (pb.has_reply_to()) {
        cmd.reply_to = net::from_proto(pb.reply_to());
    }

    cmd.type_tag = static_cast<TypeTag>(pb.type_tag());
    cmd.payload = StreamBuffer(pb.payload().begin(), pb.payload().end());
    cmd.message_id = pb.message_id();
    cmd.ask_message_id = pb.ask_message_id();
    cmd.priority = static_cast<uint8_t>(pb.priority());
    cmd.deadline_ns = pb.deadline_ns();
    cmd.flags = pb.flags();
    cmd.delay_ns = pb.delay_ns();
    cmd.schedule_handle = pb.schedule_handle();
    cmd.error_code = pb.error_code();
    cmd.detail = pb.detail();
    cmd.actor_name = pb.actor_name();
    cmd.delivery_mode = pb.delivery_mode();
    cmd.no_drop = pb.no_drop();
    cmd.emit_backpressure = pb.emit_backpressure();
    cmd.retry_max_attempts = static_cast<uint8_t>(pb.retry_max_attempts());
    cmd.retry_per_attempt_timeout_ms = pb.retry_per_attempt_timeout_ms();
    cmd.retry_initial_backoff_ms = pb.retry_initial_backoff_ms();
    cmd.retry_max_backoff_ms = pb.retry_max_backoff_ms();
    cmd.retry_backoff = static_cast<uint8_t>(pb.retry_backoff());
    cmd.retry_jitter = pb.retry_jitter();

    return result<PythonCommand>::make(std::move(cmd));
}

result<StreamBuffer>
encode_actor_failed(const ActorAddress& address, uint64_t generation,
                    const std::string& exception_type, const std::string& message,
                    const std::string& traceback, uint64_t sequence) noexcept {
    if (exception_type.size() > kMaxNameBytes) {
        return result<StreamBuffer>::make(
            error(errors::invalid_argument, "exception_type exceeds 255 bytes"));
    }
    if (message.size() > 4 * 1024) {
        return result<StreamBuffer>::make(
            error(errors::invalid_argument, "message exceeds 4 KiB"));
    }
    if (traceback.size() > kMaxTracebackBytes) {
        return result<StreamBuffer>::make(
            error(errors::invalid_argument, "traceback exceeds 16 KiB"));
    }

    internal::PbPythonActorFailed pb;
    pb.set_version(kCodecVersion);
    net::to_proto(pb.mutable_actor(), address);
    pb.set_generation(generation);
    pb.set_exception_type(exception_type);
    pb.set_message(message);
    pb.set_traceback(traceback);
    pb.set_sequence(sequence);

    std::string serialized;
    if (!pb.SerializeToString(&serialized)) {
        return result<StreamBuffer>::make(
            error(errors::unknown, "failed to encode actor failed"));
    }

    return result<StreamBuffer>::make(StreamBuffer::from_data(
        reinterpret_cast<const uint8_t*>(serialized.data()), serialized.size()));
}

result<void>
decode_actor_failed(const StreamBuffer& buffer, ActorAddress& address,
                    uint64_t& generation, std::string& exception_type,
                    std::string& message, std::string& traceback,
                    uint64_t& sequence) noexcept {
    internal::PbPythonActorFailed pb;
    if (!pb.ParseFromArray(buffer.data(), static_cast<int>(buffer.size()))) {
        return result<void>::make(
            error(errors::unknown, "failed to parse actor failed"));
    }

    if (pb.version() != kCodecVersion) {
        return result<void>::make(error(errors::invalid_argument,
                                        "unsupported actor failed version"));
    }
    if (pb.exception_type().size() > kMaxNameBytes || pb.message().size() > 4 * 1024 ||
        pb.traceback().size() > kMaxTracebackBytes) {
        return result<void>::make(error(errors::invalid_argument,
                                        "actor failed fields exceed bounds"));
    }

    if (pb.has_actor()) {
        address = net::from_proto(pb.actor());
    }
    generation = pb.generation();
    exception_type = pb.exception_type();
    message = pb.message();
    traceback = pb.traceback();
    sequence = pb.sequence();

    return result<void>::make();
}

} // namespace hpactor::python
