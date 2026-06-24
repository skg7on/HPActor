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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/actor/cli_local_actor.hpp>
#include <hpactor/cli/command/cli_session.hpp>
#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/msg/dead_letter_record.hpp>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction / static
// ---------------------------------------------------------------------------

std::string CliActor::get_history_path(const CliConfig& config) {
    if (!config.history_path.empty())
        return config.history_path;
    const char* home = getenv("HOME");
    if (!home)
        home = "/tmp";
    return std::string(home) + "/.hpactor_history";
}

CliActor::CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config)
    : InteractiveCliActor(ctx, system), config_(config) {
    formatter_ = OutputFormatter::create(config.default_format);
    pager_ = std::make_unique<Pager>(config.page_size);
}

CliActor::~CliActor() = default;

// ---------------------------------------------------------------------------
// InteractiveCliActor virtual hooks
// ---------------------------------------------------------------------------

void CliActor::print_greeting() {
    printf("HPActor CLI v1.0 — Type /help for available commands. /quit to "
           "exit.\n\n");
}

void CliActor::print_farewell() {
    printf("\n[CLI session ended]\n");
}

std::string CliActor::get_history_path() {
    return get_history_path(config_);
}

uint32_t CliActor::get_history_max() {
    return config_.history_max;
}

std::string CliActor::get_default_format() {
    return config_.default_format;
}

uint32_t CliActor::get_page_size() {
    return config_.page_size;
}

void CliActor::on_session_wired(CliSession& session) {
    session.set_cli_actor(this);
}

// ---------------------------------------------------------------------------
// Mailbox polling — block on this dedicated thread until the expected
// response tag arrives or the timeout expires.
// ---------------------------------------------------------------------------

std::optional<StreamBuffer>
CliActor::poll_for_response(TypeTag expected_tag, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;

    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage msg;
        if (mailbox()->try_pop(msg)) {
            if (msg.type_id() == expected_tag) {
                return std::move(msg).payload();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ICliCommandHost — local dispatch via try_deliver_local + mailbox poll
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliActor::inspect(ActorId target, const InspectStateRequest& req,
                  std::chrono::milliseconds timeout) {
    if (target == id()) {
        return build_self_inspect_reply(req);
    }

    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::InspectStateRequestTag, req);
        msg.set_sender_address(address());
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto payload = poll_for_response(TypeTag::InspectStateResponseTag, timeout);
    if (!payload)
        return std::nullopt;

    InspectStateReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
        return std::nullopt;
    }
    std::string wire = reply.SerializeAsString();
    InspectStateReply safe_reply;
    if (!safe_reply.ParseFromString(wire)) {
        return std::nullopt;
    }
    return safe_reply;
}

std::optional<KillReply> CliActor::kill(ActorId target, const KillRequest& req,
                                        std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;

    auto kill_deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::KillRequestTag, req);
        msg.set_sender_address(address());
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= kill_deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto payload = poll_for_response(TypeTag::KillResponseTag, timeout);
    if (!payload)
        return std::nullopt;

    KillReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
        return std::nullopt;
    }
    std::string wire = reply.SerializeAsString();
    KillReply safe_reply;
    if (!safe_reply.ParseFromString(wire)) {
        return std::nullopt;
    }
    return safe_reply;
}

std::optional<QuarantineReply>
CliActor::quarantine(ActorId target, const QuarantineRequest& req,
                     std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;

    auto q_deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::QuarantineRequestTag, req);
        msg.set_sender_address(address());
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= q_deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    auto payload = poll_for_response(TypeTag::QuarantineResponseTag, timeout);
    if (!payload)
        return std::nullopt;

    QuarantineReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size()))) {
        return std::nullopt;
    }
    std::string wire = reply.SerializeAsString();
    QuarantineReply safe_reply;
    if (!safe_reply.ParseFromString(wire)) {
        return std::nullopt;
    }
    return safe_reply;
}

// ---------------------------------------------------------------------------
// Self-inspection — builds reply inline to avoid self-deadlock
// ---------------------------------------------------------------------------

InspectStateReply
CliActor::build_self_inspect_reply(const InspectStateRequest& req) {
    InspectStateReply reply;

    auto meta = to_metadata();
    auto* pb_meta = reply.mutable_metadata();
    pb_meta->set_actor_id(meta.actor_id);
    pb_meta->set_actor_type(meta.actor_type);
    pb_meta->set_state(meta.state);
    pb_meta->set_incarnation(meta.incarnation);
    pb_meta->set_messages_processed(meta.messages_processed);
    pb_meta->set_uptime_ms(meta.uptime_ms);
    if (!meta.behavior_name.empty()) {
        pb_meta->set_behavior_name(meta.behavior_name);
    }

    if (req.include_mailbox()) {
        auto ms = mailbox_snapshot();
        auto* pb_mbox = reply.mutable_mailbox();
        pb_mbox->set_depth(ms.depth);
        pb_mbox->set_total_enqueued(ms.total_enqueued);
        pb_mbox->set_total_dequeued(ms.total_dequeued);
        pb_mbox->set_max_depth(ms.max_depth);
        pb_mbox->set_high_priority_depth(ms.high_priority_depth);
        pb_mbox->set_capacity(ms.capacity);
        pb_mbox->set_queued_bytes(ms.queued_bytes);
        pb_mbox->set_byte_capacity(ms.byte_capacity);
        pb_mbox->set_pressure_ratio_ppm(ms.pressure_ratio_ppm);
        pb_mbox->set_total_rejected(ms.total_rejected);
        pb_mbox->set_total_dropped(ms.total_dropped);
        pb_mbox->set_total_dead_letters(ms.total_dead_letters);
        pb_mbox->set_pressure_state(ms.pressure_state);
        pb_mbox->set_overflow_policy(ms.overflow_policy);
        pb_mbox->set_delivery_accepted_total(ms.delivery_accepted_total);
        pb_mbox->set_delivery_rejected_total(ms.delivery_rejected_total);
        pb_mbox->set_delivery_failed_total(ms.delivery_failed_total);
        pb_mbox->set_delivery_retryable_total(ms.delivery_retryable_total);
        if (req.include_rate_limiter()) {
            pb_mbox->set_rate_limiter_enabled(ms.rate_limiter_enabled);
            pb_mbox->set_rate_limiter_rate(ms.rate_limiter_rate);
            pb_mbox->set_rate_limiter_burst(ms.rate_limiter_burst);
            pb_mbox->set_rate_limiter_current_tokens(ms.rate_limiter_current_tokens);
            pb_mbox->set_rate_limit_blocked_total(ms.rate_limit_blocked_total);
        }
        if (req.include_admission()) {
            pb_mbox->set_admission_policy_count(ms.admission_policy_count);
            pb_mbox->set_admission_rejected_total(ms.admission_rejected_total);
            pb_mbox->set_admission_dlq_routed_total(ms.admission_dlq_routed_total);
        }
    }

    if (req.include_quarantine_info()) {
        reply.set_quarantine_enabled(quarantine_enabled());
        if (auto* lc = as_lifecycle()) {
            if (lc->is_quarantined()) {
                reply.set_quarantine_reason(
                    std::string(to_string(lc->quarantine_reason())));
            }
        }
    }

    if (req.include_state()) {
        auto blob = serialize_state();
        reply.set_state_blob(
            std::string(reinterpret_cast<const char*>(blob.data()), blob.size()));
    }

    return reply;
}

// ---------------------------------------------------------------------------
// Actor enumeration — iterates the system actor map under lock.
// ---------------------------------------------------------------------------

std::vector<ActorMeta> CliActor::enumerate(std::string_view filter) {
    std::vector<ActorMeta> result;
    system_.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (!filter.empty()) {
            std::string type_name(actor.type_name().data(),
                                  actor.type_name().size());
            if (type_name.find(filter) == std::string::npos)
                return;
        }
        auto meta = actor.to_metadata();
        result.push_back(std::move(meta));
    });
    return result;
}

// execute_path is inline in header (returns false — local host).
// dlq_replay is the only remaining ISystemCliHost method with a body.

result<void> CliActor::dlq_replay(uint32_t index, ActorId target) {
    auto* dlq = system_.dead_letter_queue();
    if (!dlq)
        return result<void>::make(
            error(errors::actor_not_found, "DLQ not configured"));

    mailbox::DeadLetterRecord record;
    if (!dlq->try_pop_at(index, record))
        return result<void>::make(
            error(errors::invalid_argument, "DLQ index out of range"));

    TypedMessage msg(record.type_tag, std::move(record.payload_sample));
    msg.set_sender_address(address());
    auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
    if (!enqueue_result.accepted())
        return result<void>::make(
            error(errors::mailbox_full, "replay delivery failed"));

    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface
// ---------------------------------------------------------------------------

result<void> CliActor::drain() {
    return system_.shutdown();
}

result<void> CliActor::shutdown() {
    return system_.shutdown();
}

} // namespace cli
} // namespace hpactor
