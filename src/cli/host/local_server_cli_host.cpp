// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cli/host/local_server_cli_host.hpp>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/command/command_tree_builder.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

LocalServerCliHost::LocalServerCliHost(ActorSystem& system) : system_(system) {}

// ---------------------------------------------------------------------------
// poll_for_response
// ---------------------------------------------------------------------------

std::optional<adt::StreamBuffer>
LocalServerCliHost::poll_for_response(mailbox::MPSCActorMailbox<TypedMessage>* mbox,
                                      TypeTag expected_tag,
                                      std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage msg;
        if (mbox->try_pop(msg)) {
            if (msg.type_id() == expected_tag)
                return std::move(msg).payload();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// ICliCommandHost — inspect
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
LocalServerCliHost::inspect(ActorId target, const InspectStateRequest& req,
                            mailbox::MPSCActorMailbox<TypedMessage>* mbox,
                            const ActorAddress& self_addr,
                            std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::InspectStateRequestTag, req);
        msg.set_sender_address(self_addr);
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto payload =
        poll_for_response(mbox, TypeTag::InspectStateResponseTag, timeout);
    if (!payload)
        return std::nullopt;
    InspectStateReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
        return std::nullopt;
    std::string wire = reply.SerializeAsString();
    InspectStateReply safe_reply;
    if (!safe_reply.ParseFromString(wire))
        return std::nullopt;
    return safe_reply;
}

// ---------------------------------------------------------------------------
// ICliCommandHost — kill
// ---------------------------------------------------------------------------

std::optional<KillReply>
LocalServerCliHost::kill(ActorId target, const KillRequest& req,
                         mailbox::MPSCActorMailbox<TypedMessage>* mbox,
                         const ActorAddress& self_addr,
                         std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;
    auto kill_deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::KillRequestTag, req);
        msg.set_sender_address(self_addr);
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= kill_deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto payload = poll_for_response(mbox, TypeTag::KillResponseTag, timeout);
    if (!payload)
        return std::nullopt;
    KillReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
        return std::nullopt;
    std::string wire = reply.SerializeAsString();
    KillReply safe_reply;
    if (!safe_reply.ParseFromString(wire))
        return std::nullopt;
    return safe_reply;
}

// ---------------------------------------------------------------------------
// ICliCommandHost — quarantine
// ---------------------------------------------------------------------------

std::optional<QuarantineReply>
LocalServerCliHost::quarantine(ActorId target, const QuarantineRequest& req,
                               mailbox::MPSCActorMailbox<TypedMessage>* mbox,
                               const ActorAddress& self_addr,
                               std::chrono::milliseconds timeout) {
    auto actor = system_.get_actor(target);
    if (!actor)
        return std::nullopt;
    auto q_deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        TypedMessage msg(TypeTag::QuarantineRequestTag, req);
        msg.set_sender_address(self_addr);
        auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
        if (enqueue_result.accepted())
            break;
        if (std::chrono::steady_clock::now() >= q_deadline)
            return std::nullopt;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    auto payload = poll_for_response(mbox, TypeTag::QuarantineResponseTag, timeout);
    if (!payload)
        return std::nullopt;
    QuarantineReply reply;
    if (!reply.ParseFromArray(payload->data(), static_cast<int>(payload->size())))
        return std::nullopt;
    std::string wire = reply.SerializeAsString();
    QuarantineReply safe_reply;
    if (!safe_reply.ParseFromString(wire))
        return std::nullopt;
    return safe_reply;
}

// ---------------------------------------------------------------------------
// ICliCommandHost — enumerate
// ---------------------------------------------------------------------------

std::vector<ActorMeta> LocalServerCliHost::enumerate(std::string_view filter) {
    std::vector<ActorMeta> result;
    system_.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        if (!filter.empty()) {
            std::string tn(actor.type_name().data(), actor.type_name().size());
            if (tn.find(filter) == std::string::npos)
                return;
        }
        result.push_back(actor.to_metadata());
    });
    return result;
}

// ---------------------------------------------------------------------------
// ISystemCliHost — dlq_replay
// ---------------------------------------------------------------------------

result<void> LocalServerCliHost::dlq_replay(uint32_t index, ActorId target,
                                            const ActorAddress& self_addr) {
    auto* dlq = system_.dead_letter_queue();
    if (!dlq)
        return result<void>::make(
            error(errors::actor_not_found, "DLQ not configured"));

    mailbox::DeadLetterRecord record;
    if (!dlq->try_pop_at(index, record))
        return result<void>::make(
            error(errors::invalid_argument, "DLQ index out of range"));

    TypedMessage msg(record.type_tag, std::move(record.payload_sample));
    msg.set_sender_address(self_addr);
    auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
    if (!enqueue_result.accepted())
        return result<void>::make(
            error(errors::mailbox_full, "replay delivery failed"));

    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost — drain / shutdown
// ---------------------------------------------------------------------------

result<void> LocalServerCliHost::drain() {
    return system_.shutdown();
}

result<void> LocalServerCliHost::shutdown() {
    return system_.shutdown();
}

// ---------------------------------------------------------------------------
// Shared utilities
// ---------------------------------------------------------------------------

std::unique_ptr<CommandNode> LocalServerCliHost::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*root);
    return root;
}

cli::ActorMeta
LocalServerCliHost::make_metadata(ActorId id, std::string_view type_name,
                                  bool running) const {
    cli::ActorMeta m;
    m.actor_id = id.value();
    m.actor_type = std::string(type_name);
    m.state = running ? "Running" : "Stopped";
    return m;
}

} // namespace cli
} // namespace hpactor
