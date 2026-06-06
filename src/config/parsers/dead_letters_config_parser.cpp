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
#include <hpactor/msg/dead_letter_record.hpp>

#include <string>

namespace hpactor::config {
namespace {

static hpactor::mailbox::DeadLetterOverflowPolicy
parse_dl_overflow_policy(const std::string& s) {
    if (s == "drop_newest_record")
        return hpactor::mailbox::DeadLetterOverflowPolicy::DropNewestRecord;
    if (s == "metadata_only")
        return hpactor::mailbox::DeadLetterOverflowPolicy::MetadataOnly;
    return hpactor::mailbox::DeadLetterOverflowPolicy::DropOldestRecord;
}

class DeadLettersConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.dead_letters";
    static constexpr int kOrder = 95;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto dt = system.table("dead_letters");
        if (!dt.valid())
            return result<void>::make();

        out.dead_letters.enabled = dt.read_bool("enabled", true);
        out.dead_letters.capacity = dt.read_uint32("capacity", 4096);
        out.dead_letters.byte_capacity = dt.read_uint32("byte_capacity", 0);
        out.dead_letters.max_payload_sample_bytes =
            dt.read_uint32("max_payload_sample_bytes", 512);
        out.dead_letters.overflow_policy =
            parse_dl_overflow_policy(dt.read_string("overflow_policy", "drop_"
                                                                       "oldest_"
                                                                       "recor"
                                                                       "d"));
        out.dead_letters.store_payload = dt.read_bool("store_payload", true);
        out.dead_letters.alert_on_first_failure =
            dt.read_bool("alert_on_first_failure", false);
        out.dead_letters.alert_threshold_per_minute =
            dt.read_uint32("alert_threshold_per_minute", 100);

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<DeadLettersConfigParser> kRegisterDeadLettersConfigParser;

} // anonymous namespace
} // namespace hpactor::config
