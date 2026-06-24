// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/tracing/trace_config.hpp>
#include <hpactor/tracing/trace_manager.hpp>

#include <map>
#include <string>

namespace hpactor {
namespace cli {
namespace {

class TracingStatusCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "tracing/status";
    }
    std::string_view help_text() const noexcept override {
        return "Show distributed tracing subsystem status";
    }
    int order() const noexcept override {
        return 700;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* sys = ctx.system;
        if (!sys) {
            ctx.output->error("Internal error: no actor system");
            return result<void>::make();
        }
        auto* tm = sys->trace_manager();
        if (!tm) {
            ctx.output->raw("Tracing subsystem is not enabled.");
            return result<void>::make();
        }

        ctx.output->header("Tracing Status");

        std::map<std::string, std::string> kv;
        kv["Enabled"] = tm->enabled() ? "yes" : "no";
        auto& cfg = tm->config();
        if (tm->enabled()) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.2f (%.0f%%)", cfg.sample_ratio,
                     cfg.sample_ratio * 100.0);
            kv["Sampling rate"] = buf;
            kv["Ring buffer capacity"] = std::to_string(cfg.ring_buffer_capacity);
            kv["Spans dropped"] = std::to_string(tm->spans_dropped());
            kv["Service name"] = cfg.service_name;
            kv["Record actor receive"] =
                cfg.record_actor_receive_spans ? "yes" : "no";
            kv["Record local sends"] =
                cfg.record_local_producer_spans ? "yes" : "no";
        }
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

const CommandRegistration<TracingStatusCommand> kRegisterTracingStatus;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
