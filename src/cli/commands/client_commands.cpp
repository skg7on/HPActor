// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/cli/cli_proto_server_actor.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>

#include <charconv>
#include <string>

namespace hpactor {
namespace cli {
namespace {

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
        if (ctx.system_host &&
            ctx.system_host->execute_path("client/list", {}, {}, *ctx.output))
            return result<void>::make();
        if (ctx.cli_proto_server) {
            ctx.output->raw(ctx.cli_proto_server->list_clients());
            return result<void>::make();
        }
        ctx.output->error("Client management not available on this host");
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

        if (ctx.system_host &&
            ctx.system_host->execute_path("client/" + *seqno_str + "/close", {},
                                          {}, *ctx.output))
            return result<void>::make();
        if (ctx.cli_proto_server) {
            if (ctx.cli_proto_server->close_client(seqno))
                ctx.output->raw("Client " + *seqno_str + " disconnected.\n");
            else
                ctx.output->error("Client " + *seqno_str + " not found.");
            return result<void>::make();
        }
        ctx.output->error("Client management not available on this host");
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

        if (ctx.system_host &&
            ctx.system_host->execute_path("client/" + *seqno_str + "/history",
                                          {}, {}, *ctx.output))
            return result<void>::make();
        if (ctx.cli_proto_server) {
            ctx.output->raw(ctx.cli_proto_server->client_history(seqno));
            return result<void>::make();
        }
        ctx.output->error("Client management not available on this host");
        return result<void>::make();
    }
};

const CommandRegistration<ClientListCommand> kRegisterClientList;
const CommandRegistration<ClientCloseCommand> kRegisterClientClose;
const CommandRegistration<ClientHistoryCommand> kRegisterClientHistory;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
