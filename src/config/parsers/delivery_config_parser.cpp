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

#include <hpactor/config/toml_config_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/mailbox/delivery_mode.hpp>

#include <string>

namespace hpactor::config {
namespace {

static hpactor::mailbox::DeliveryMode
parse_delivery_mode(const std::string& s) {
    if (s == "observable_best_effort")
        return hpactor::mailbox::DeliveryMode::ObservableBestEffort;
    if (s == "at_least_once")
        return hpactor::mailbox::DeliveryMode::AtLeastOnce;
    if (s == "durable_at_least_once")
        return hpactor::mailbox::DeliveryMode::DurableAtLeastOnce;
    return hpactor::mailbox::DeliveryMode::BestEffort;
}

class DeliveryConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.delivery";
    static constexpr int kOrder = 95;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto dt = system.table("delivery");
        if (!dt.valid())
            return result<void>::make();

        out.delivery.default_mode =
            parse_delivery_mode(dt.read_string("default_mode", "best_effort"));
        out.delivery.max_retries = dt.read_uint32("max_retries", 3);
        out.delivery.retry_backoff_ms = dt.read_uint32("retry_backoff_ms", 100);
        out.delivery.retry_backoff_max_ms =
            dt.read_uint32("retry_backoff_max_ms", 10000);
        out.delivery.dedup_window_ms =
            dt.read_uint32("dedup_window_ms", 300000);
        out.delivery.dedup_max_entries =
            dt.read_uint32("dedup_max_entries", 65536);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<DeliveryConfigParser>
    kRegisterDeliveryConfigParser;

} // anonymous namespace
} // namespace hpactor::config
