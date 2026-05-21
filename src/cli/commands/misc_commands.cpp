// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>

namespace hpactor {
namespace cli {
namespace {

class MetricsShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "metrics/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show current metrics snapshot";
    }
    int order() const noexcept override {
        return 100;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Metrics");
        ctx.output->raw("metrics show — not yet implemented");
        return result<void>::make();
    }
};

class TopologyShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "topology/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show topology tree";
    }
    int order() const noexcept override {
        return 100;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Topology");
        ctx.output->raw("topology show — not yet implemented");
        return result<void>::make();
    }
};

const CommandRegistration<MetricsShowCommand> kRegisterMetricsShow;
const CommandRegistration<TopologyShowCommand> kRegisterTopologyShow;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
