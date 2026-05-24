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
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

namespace hpactor::config {
namespace {

// ---------------------------------------------------------------------------
// DispatchPolicy string parsing
// ---------------------------------------------------------------------------
static DispatchPolicy parse_dispatch_policy(const std::string& s) {
    if (s == "DedicatedThread")
        return DispatchPolicy::DedicatedThread;
    if (s == "DedicatedPool")
        return DispatchPolicy::DedicatedPool;
    return DispatchPolicy::Cooperative;
}

// ---------------------------------------------------------------------------
// OverflowPolicy string parsing
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// Parse [[dispatcher]] from a toml table
// ---------------------------------------------------------------------------
static DispatcherDef parse_dispatcher(const TomlTableView& tbl) {
    DispatcherDef def;
    def.name = tbl.read_string("name");
    def.threads = static_cast<uint16_t>(tbl.read_uint32("threads", 1));

    tbl.for_each_integer_array("cpu_affinity", [&](int64_t val) {
        if (val >= 0 && val <= 255)
            def.cpu_affinity.push_back(static_cast<uint8_t>(val));
    });
    return def;
}

// ---------------------------------------------------------------------------
// Parse [[actor]] from a toml table
// ---------------------------------------------------------------------------
static ActorDef parse_actor(const TomlTableView& tbl) {
    ActorDef def;
    def.id = tbl.read_string("id");
    def.behavior = tbl.read_string("behavior");
    def.supervisor = tbl.read_string("supervisor");
    def.dispatcher = tbl.read_string("dispatcher");
    def.mailbox_capacity = tbl.read_uint32("mailbox_capacity");
    def.dispatch_policy =
        parse_dispatch_policy(tbl.read_string("dispatch_policy", "Cooperativ"
                                                                 "e"));

    auto resources = tbl.table("resources");
    if (resources.valid()) {
        def.resources.slab_class_bytes = resources.read_uint32("slab_class_"
                                                               "bytes");
        def.resources.max_memory_kb = resources.read_uint32("max_memory_kb");
    }

    auto mailbox = tbl.table("mailbox");
    if (mailbox.valid()) {
        def.mailbox.policy =
            parse_overflow_policy(mailbox.read_string("policy", "reject_"
                                                                "newest"));
        def.mailbox.priority_aware = mailbox.read_bool("priority_aware", false);
        def.mailbox.max_overflow_depth =
            mailbox.read_uint32("max_overflow_depth", 0);
    }

    auto args = tbl.table("args");
    if (args.valid()) {
        args.for_each_entry([&](std::string_view key, TomlValueView val) {
            std::string key_str{key};
            if (val.is_string())
                def.args[key_str] = val.as_string("");
            else if (val.is_integer())
                def.args[key_str] = std::to_string(val.as_int64(0));
            else if (val.is_floating_point())
                def.args[key_str] = std::to_string(val.as_double(0.0));
            else if (val.is_boolean())
                def.args[key_str] = val.as_bool(false) ? "true" : "false";
        });
    }

    auto quarantine = tbl.table("quarantine");
    if (quarantine.valid()) {
        def.quarantine.enabled = quarantine.read_bool("enabled", false);
        def.quarantine.escalate_on_max_restarts =
            quarantine.read_bool("escalate_on_max_restarts", true);
        def.quarantine.failure_rate_threshold =
            quarantine.read_uint32("failure_rate_threshold", 0);
        def.quarantine.timeout_rate_threshold =
            quarantine.read_uint32("timeout_rate_threshold", 0);
        def.quarantine.mailbox_pressure_threshold = static_cast<float>(
            quarantine.read_double("mailbox_pressure_threshold", 0.0));
        def.quarantine.cooldown_period =
            std::chrono::milliseconds(static_cast<int64_t>(
                quarantine.read_uint32("cooldown_period_ms", 30000)));
        def.quarantine.observation_window =
            std::chrono::milliseconds(static_cast<int64_t>(
                quarantine.read_uint32("observation_window_ms", 10000)));
        def.quarantine.max_circuit_trips =
            quarantine.read_uint32("max_circuit_trips", 3);
    }

    return def;
}

// ---------------------------------------------------------------------------
// Topology document parser
// ---------------------------------------------------------------------------
class TopologyConfigParser final : public ITomlDocumentConfigParser {
  public:
    static constexpr std::string_view kName = "topology.document";
    static constexpr int kOrder = 0;

    std::string_view name() const noexcept override {
        return kName;
    }
    int order() const noexcept override {
        return kOrder;
    }

    result<void> parse(const TomlTableView& root, TomlFileData& out,
                       TomlParseContext& /*ctx*/) const override {
        // Parse [[dispatcher]]
        root.for_each_table_array("dispatcher", [&](TomlTableView disp_tbl) {
            out.dispatchers.emplace_back(parse_dispatcher(disp_tbl));
        });

        // Parse [template.*]
        root.for_each_subtable("template", [&](std::string_view tmpl_name,
                                               TomlTableView tmpl_tbl) {
            out.templates[std::string{tmpl_name}] = parse_actor(tmpl_tbl);
        });

        // Parse [[actor]]
        root.for_each_table_array("actor", [&](TomlTableView actor_tbl) {
            TomlRawActor raw;
            raw.def = parse_actor(actor_tbl);
            raw.inherits = actor_tbl.read_string("inherits");
            out.actors.emplace_back(std::move(raw));
        });

        return result<void>::make();
    }
};

const TomlDocumentParserRegistration<TopologyConfigParser> kRegisterTopologyConfigParser;

} // anonymous namespace
} // namespace hpactor::config
