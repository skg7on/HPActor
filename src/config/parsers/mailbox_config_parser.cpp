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
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <string>

namespace hpactor::config {
namespace {

// BackpressureMode string parsing (same logic as topology parser)
static hpactor::mailbox::BackpressureMode
parse_backpressure_mode(const std::string& s) {
    if (s == "disabled")
        return hpactor::mailbox::BackpressureMode::Disabled;
    if (s == "local")
        return hpactor::mailbox::BackpressureMode::LocalSignal;
    if (s == "remote")
        return hpactor::mailbox::BackpressureMode::RemoteSignal;
    return hpactor::mailbox::BackpressureMode::LocalAndRemoteSignal;
}

// OverflowPolicy string parsing
static hpactor::mailbox::OverflowPolicy
parse_overflow_policy(const std::string& s) {
    if (s == "drop_newest")
        return hpactor::mailbox::OverflowPolicy::DropNewest;
    if (s == "drop_oldest")
        return hpactor::mailbox::OverflowPolicy::DropOldest;
    if (s == "drop_lowest_priority")
        return hpactor::mailbox::OverflowPolicy::DropLowestPriority;
    if (s == "dead_letter")
        return hpactor::mailbox::OverflowPolicy::DeadLetter;
    if (s == "spill_to_overflow_queue")
        return hpactor::mailbox::OverflowPolicy::SpillToOverflowQueue;
    if (s == "signal_only")
        return hpactor::mailbox::OverflowPolicy::SignalOnly;
    if (s == "block_when_allowed")
        return hpactor::mailbox::OverflowPolicy::BlockWhenAllowed;
    return hpactor::mailbox::OverflowPolicy::RejectNewest;
}

class MailboxConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.mailbox";
    static constexpr int kOrder = 90;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto mt = system.table("mailbox");
        if (!mt.valid())
            return result<void>::make();

        out.mailbox.default_capacity = mt.read_uint32("default_capacity", 1024);
        out.mailbox.default_byte_capacity = static_cast<uint64_t>(
            mt.value("default_byte_capacity").as_int64(0));
        out.mailbox.default_policy =
            parse_overflow_policy(mt.read_string("default_policy", "reject_"
                                                                   "newest"));
        out.mailbox.high_watermark = mt.read_double("high_watermark", 0.80);
        out.mailbox.low_watermark = mt.read_double("low_watermark", 0.50);
        out.mailbox.protected_system_messages =
            mt.read_uint32("protected_system_messages", 32);
        out.mailbox.backpressure_mode =
            parse_backpressure_mode(mt.read_string("backpressure", "local_and_"
                                                                   "remote"));
        out.mailbox.max_overflow_depth =
            mt.read_uint32("max_overflow_depth", 0);
        out.mailbox.signal_min_interval_ms =
            mt.read_uint32("signal_min_interval_ms", 100);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<MailboxConfigParser> kRegisterMailboxConfigParser;

} // anonymous namespace
} // namespace hpactor::config
