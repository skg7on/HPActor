// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cli/actor/cli_client_actor.hpp>
#include <hpactor/cli/actor/cli_legacy_server_actor.hpp>
#include <hpactor/cli/actor/cli_proto_server_actor.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>

#include <charconv>
#include <string>

namespace hpactor {
namespace cli {
namespace {

/// Find a CliProtoServerActor in the ActorSystem.  Needed when the command
/// runs in the local stdin CLI (CliActor) session — ctx.cli_proto_server is
/// only set for remote client sessions handled by the proto server itself.
static CliProtoServerActor* find_proto_server(ActorSystem* sys) {
    if (!sys)
        return nullptr;
    CliProtoServerActor* found = nullptr;
    sys->for_each_actor([&](ActorId, AbstractActor& actor) {
        if (!found && actor.type_name() ==
                          std::string_view(CliProtoServerActor::kActorTypeName))
            found = static_cast<CliProtoServerActor*>(&actor);
    });
    return found;
}

/// Find a CliLegacyServerActor in the ActorSystem.  Needed when the command
/// runs in the local stdin CLI (CliActor) session.
static CliLegacyServerActor* find_legacy_server(ActorSystem* sys) {
    if (!sys)
        return nullptr;
    CliLegacyServerActor* found = nullptr;
    sys->for_each_actor([&](ActorId, AbstractActor& actor) {
        if (!found && actor.type_name() ==
                          std::string_view(CliLegacyServerActor::kActorTypeName))
            found = static_cast<CliLegacyServerActor*>(&actor);
    });
    return found;
}

/// Helper: try to find any server (proto first, then legacy) in the
/// ActorSystem and call \p fn on it.  Returns false if no server found.
template <typename Fn> static bool try_with_server(CommandContext& ctx, Fn fn) {
    if (auto* srv = ctx.cli_proto_server) {
        fn(srv);
        return true;
    }
    if (auto* srv = ctx.cli_server_actor) {
        fn(srv);
        return true;
    }
    if (auto* srv = find_proto_server(ctx.system)) {
        fn(srv);
        return true;
    }
    if (auto* srv = find_legacy_server(ctx.system)) {
        fn(srv);
        return true;
    }
    return false;
}

class ClientListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "client/list";
    }
    std::string_view help_text() const noexcept override {
        return "List connected CLI clients";
    }
    int order() const noexcept override {
        return 600;
    }

    result<void> execute(CommandContext& ctx) const override {
        // 1. Client-side: show only this client's own connection status.
        if (auto* client = ctx.cli_client_actor) {
            ctx.output->raw(client->list_clients());
            return result<void>::make();
        }
        // 2. Server-side: show all connected clients.
        bool found = try_with_server(
            ctx, [&ctx](auto* srv) { ctx.output->raw(srv->list_clients()); });
        if (found)
            return result<void>::make();
        // 3. No server available.
        ctx.output->error("No CLI server running — client management "
                          "not available.");
        return result<void>::make();
    }
};

class ClientCloseCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "client/<seqno>/close";
    }
    std::string_view help_text() const noexcept override {
        return "Disconnect a client by seqno";
    }
    int order() const noexcept override {
        return 610;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto seqno_str = ctx.get_param("<seqno>");
        if (!seqno_str) {
            ctx.output->error("Usage: /client <seqno> close");
            return result<void>::make();
        }
        uint64_t val = 0;
        auto [ptr, ec] = std::from_chars(
            seqno_str->data(), seqno_str->data() + seqno_str->size(), val);
        if (ec != std::errc{} || val > UINT32_MAX) {
            ctx.output->error("Invalid seqno: " + *seqno_str);
            return result<void>::make();
        }
        uint32_t seqno = static_cast<uint32_t>(val);

        // 1. Client-side: disconnect self from server.
        if (auto* client = ctx.cli_client_actor) {
            if (client->close_client(seqno))
                ctx.output->raw("Disconnected from server.\n");
            else
                ctx.output->error("Not connected to any server.");
            return result<void>::make();
        }
        // 2. Server-side: close a specific client by seqno.
        bool found = try_with_server(ctx, [&ctx, seqno, &seqno_str](auto* srv) {
            if (srv->close_client(seqno))
                ctx.output->raw("Client " + *seqno_str + " disconnected.\n");
            else
                ctx.output->error("Client " + *seqno_str + " not found.");
        });
        if (found)
            return result<void>::make();
        ctx.output->error("No CLI server running.");
        return result<void>::make();
    }
};

class ClientHistoryCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "client/<seqno>/history";
    }
    std::string_view help_text() const noexcept override {
        return "Show command history for a client";
    }
    int order() const noexcept override {
        return 620;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto seqno_str = ctx.get_param("<seqno>");
        if (!seqno_str) {
            ctx.output->error("Usage: /client <seqno> history");
            return result<void>::make();
        }
        uint64_t val = 0;
        auto [ptr, ec] = std::from_chars(
            seqno_str->data(), seqno_str->data() + seqno_str->size(), val);
        if (ec != std::errc{} || val > UINT32_MAX) {
            ctx.output->error("Invalid seqno: " + *seqno_str);
            return result<void>::make();
        }
        uint32_t seqno = static_cast<uint32_t>(val);

        // 1. Client-side: history is tracked on the server.
        if (auto* client = ctx.cli_client_actor) {
            ctx.output->raw(client->client_history(seqno));
            return result<void>::make();
        }
        // 2. Server-side: show history for a specific client.
        bool found = try_with_server(ctx, [&ctx, seqno](auto* srv) {
            ctx.output->raw(srv->client_history(seqno));
        });
        if (found)
            return result<void>::make();
        ctx.output->error("No CLI server running.");
        return result<void>::make();
    }
};

const CommandRegistration<ClientListCommand> kRegisterClientList;
const CommandRegistration<ClientCloseCommand> kRegisterClientClose;
const CommandRegistration<ClientHistoryCommand> kRegisterClientHistory;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
