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
#include <hpactor/log/log_category.hpp>
#include <hpactor/log/log_level.hpp>

namespace hpactor::config {
namespace {

class LoggingConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.logging";
    static constexpr int kOrder = 110;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& system, SystemDef& out,
                       TomlParseContext& /*ctx*/) const override {
        auto lt = system.table("logging");
        if (!lt.valid())
            return result<void>::make();

        out.logging.enabled = lt.read_bool("enabled", true);

        // default_level (string → LogLevel)
        auto lvl_str = lt.read_string("default_level", "info");
        if (auto parsed = hpactor::log::parse_level(lvl_str); parsed.has_value())
            out.logging.default_level = parsed.value();

        // format (string → LogFormat)
        auto fmt_str = lt.read_string("format", "json");
        if (fmt_str == "text")
            out.logging.format = hpactor::log::LogFormat::kText;
        else
            out.logging.format = hpactor::log::LogFormat::kJson;

        out.logging.ring_buffer_capacity =
            lt.read_uint32("ring_buffer_capacity", 65536);

        // flush_on_level (string → LogLevel)
        auto flush_str = lt.read_string("flush_on_level", "error");
        if (auto parsed = hpactor::log::parse_level(flush_str); parsed.has_value())
            out.logging.flush_on_level = parsed.value();

        out.logging.file_path = lt.read_string("file_path", "");

        // drop_policy (string → DropPolicy)
        auto drop_str = lt.read_string("drop_policy", "drop_newest");
        if (drop_str == "drop_newest")
            out.logging.drop_policy = hpactor::log::DropPolicy::kDropNewest;

        // sinks (array of strings → vector<LogSinkKind>)
        lt.for_each_string_array("sinks", [&](std::string_view s) {
            if (s == "stderr")
                out.logging.sinks.emplace_back(hpactor::log::LogSinkKind::kStderr);
            else if (s == "file")
                out.logging.sinks.emplace_back(hpactor::log::LogSinkKind::kFile);
            else if (s == "rotating_file")
                out.logging.sinks.emplace_back(
                    hpactor::log::LogSinkKind::kRotatingFile);
        });

        // [system.logging.levels] sub-table
        auto levels_tbl = lt.table("levels");
        if (levels_tbl.valid()) {
            levels_tbl.for_each_entry([&](std::string_view key, TomlValueView val) {
                if (!val.is_string())
                    return;
                auto cat = hpactor::log::parse_category(key);
                if (!cat.has_value())
                    return;
                auto lvl =
                    hpactor::log::parse_level(std::string_view{val.as_string("")});
                if (!lvl.has_value())
                    return;
                auto idx = static_cast<size_t>(cat.value());
                out.logging.levels[idx] = lvl.value();
            });
        }

        // [system.logging.rotating_file] sub-table
        auto rf = lt.table("rotating_file");
        if (rf.valid()) {
            out.logging.rotating_file.path = rf.read_string("path", "");
            out.logging.rotating_file.max_bytes =
                static_cast<uint64_t>(rf.read_uint32("max_bytes", 104857600));
            out.logging.rotating_file.max_files = rf.read_uint32("max_files", 5);
        }

        return result<void>::make();
    }
};

const TomlSystemParserRegistration<LoggingConfigParser> kRegisterLoggingConfigParser;

} // anonymous namespace
} // namespace hpactor::config
