// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/types/failure_reason.hpp>

#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class FailureReasonsCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "failure/reasons";
    }
    std::string_view help_text() const noexcept override {
        return "List all canonical failure reasons";
    }
    int order() const noexcept override {
        return 600;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Canonical Failure Reasons");

        std::vector<std::string> cols = {"Reason", "Code", "Retryable"};
        std::vector<std::vector<std::string>> rows;

        auto add_row = [&](FailureReason r, uint8_t code) {
            rows.push_back({std::string(to_string(r)), std::to_string(code),
                            retryable(r) ? "yes" : "no"});
        };

        add_row(FailureReason::NoRoute, 0);
        add_row(FailureReason::NodeUnavailable, 1);
        add_row(FailureReason::ActorDead, 10);
        add_row(FailureReason::ActorNotReady, 11);
        add_row(FailureReason::Quarantined, 12);
        add_row(FailureReason::CircuitOpen, 13);
        add_row(FailureReason::MailboxFull, 20);
        add_row(FailureReason::OutboundQueueFull, 21);
        add_row(FailureReason::MemoryPressure, 22);
        add_row(FailureReason::Expired, 30);
        add_row(FailureReason::Timeout, 31);
        add_row(FailureReason::RejectedByPolicy, 40);
        add_row(FailureReason::Dropped, 41);
        add_row(FailureReason::MailboxClosed, 42);
        add_row(FailureReason::SerializationError, 50);
        add_row(FailureReason::TransportError, 51);
        add_row(FailureReason::FrameRejected, 52);
        add_row(FailureReason::Duplicate, 60);
        add_row(FailureReason::Draining, 70);
        add_row(FailureReason::ShuttingDown, 71);
        add_row(FailureReason::RetryExhausted, 80);
        add_row(FailureReason::SpawnFailed, 90);
        add_row(FailureReason::Unknown, 255);

        ctx.output->table(cols, rows);
        return result<void>::make();
    }
};

class FailureSummaryCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "failure/summary";
    }
    std::string_view help_text() const noexcept override {
        return "Show failure subsystem status";
    }
    int order() const noexcept override {
        return 610;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Failure Subsystem Status");

        std::map<std::string, std::string> kv;
        kv["FailureReason values"] = "23";
        kv["FailureSource values"] = "12";
        kv["DLQ mapping"] = "13 DeadLetterReason codes mapped";
        kv["Spawn mapping"] = "6 spawn_errors codes mapped";
        kv["EnqueueResultCode mapping"] = "9 mailbox codes mapped";
        kv["Delivery failure metric"] = "kDeliveryFailure wired in "
                                        "try_deliver_local";
        kv["Phase"] = "1-4 complete";

        ctx.output->key_value(kv);
        ctx.output->raw("Use /failure reasons for the full reason table.");
        ctx.output->raw("Use /actor <id> show for per-actor mailbox/depth "
                        "stats.");
        return result<void>::make();
    }
};

const CommandRegistration<FailureReasonsCommand> kRegisterFailureReasons;
const CommandRegistration<FailureSummaryCommand> kRegisterFailureSummary;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
