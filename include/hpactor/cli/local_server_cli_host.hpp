// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class OutputFormatter;
struct CommandNode;

/// \brief Shared local implementations of the three CLI host interfaces.
///
/// All local CLI server actors (CliProtoServerActor, CliLegacyServerActor,
/// CliHttpServerActor) delegate their ICliCommandHost, ISystemCliHost, and
/// ILifecycleCliHost implementations to this class, eliminating ~500 lines
/// of copy-pasted polling loops and ActorSystem queries.
///
/// Methods that require mailbox polling (inspect, kill, quarantine) accept
/// a \c MPSCActorMailbox* from the owning actor so this class remains
/// actor-agnostic.
class LocalServerCliHost {
  public:
    explicit LocalServerCliHost(ActorSystem& system);

    // ── ICliCommandHost ──────────────────────────────────────────────

    std::optional<class InspectStateReply>
    inspect(ActorId target, const class InspectStateRequest& req,
            class mailbox::MPSCActorMailbox<class TypedMessage>* mbox,
            const struct ActorAddress& self_addr,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    std::optional<class KillReply>
    kill(ActorId target, const class KillRequest& req,
         class mailbox::MPSCActorMailbox<class TypedMessage>* mbox,
         const struct ActorAddress& self_addr,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    std::optional<class QuarantineReply>
    quarantine(ActorId target, const class QuarantineRequest& req,
               class mailbox::MPSCActorMailbox<class TypedMessage>* mbox,
               const struct ActorAddress& self_addr,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000));

    std::vector<ActorMeta> enumerate(std::string_view filter = "");

    // ── ISystemCliHost ───────────────────────────────────────────────

    result<void> dlq_replay(uint32_t index, ActorId target,
                            const struct ActorAddress& self_addr);

    // ── ILifecycleCliHost ────────────────────────────────────────────

    result<void> drain();
    result<void> shutdown();

    // ── Shared utilities ─────────────────────────────────────────────

    /// Build a command tree from the global CommandRegistry.
    std::unique_ptr<CommandNode> build_command_tree();

    /// Build to_metadata() response for the given actor id + type name.
    cli::ActorMeta
    make_metadata(ActorId id, std::string_view type_name, bool running) const;

  private:
    /// Poll the mailbox for a response with a specific TypeTag.
    static std::optional<adt::StreamBuffer>
    poll_for_response(mailbox::MPSCActorMailbox<TypedMessage>* mbox,
                      TypeTag expected_tag, std::chrono::milliseconds timeout);

    ActorSystem& system_;
};

} // namespace cli
} // namespace hpactor
