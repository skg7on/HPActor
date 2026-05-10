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

#include <hpactor/config/toml_parser.hpp>
#include <hpactor/log/log_category.hpp>
#include <hpactor/log/log_level.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <toml.hpp>

#include <deque>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

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
// BackpressureMode string parsing
// ---------------------------------------------------------------------------
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

// ---------------------------------------------------------------------------
// DeadLetterOverflowPolicy string parsing
// ---------------------------------------------------------------------------
static hpactor::mailbox::DeadLetterOverflowPolicy
parse_dl_overflow_policy(const std::string& s) {
    if (s == "drop_newest_record")
        return hpactor::mailbox::DeadLetterOverflowPolicy::DropNewestRecord;
    if (s == "metadata_only")
        return hpactor::mailbox::DeadLetterOverflowPolicy::MetadataOnly;
    return hpactor::mailbox::DeadLetterOverflowPolicy::DropOldestRecord;
}

// ---------------------------------------------------------------------------
// Glob expansion — simple *-only pattern via fs::directory_iterator
// ---------------------------------------------------------------------------
static std::vector<std::string> expand_glob(const std::string& pattern) {
    std::vector<std::string> results;
    fs::path p(pattern);

    if (p.is_absolute() && fs::exists(p)) {
        results.push_back(p.string());
        return results;
    }

    fs::path parent = p.parent_path();
    if (parent.empty())
        parent = ".";
    std::string fname = p.filename().string();

    auto star_pos = fname.find('*');
    if (star_pos == std::string::npos) {
        if (fs::exists(p))
            results.push_back(p.string());
        return results;
    }

    std::string prefix = fname.substr(0, star_pos);
    std::string suffix = fname.substr(star_pos + 1);

    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(parent, ec)) {
        if (ec)
            break;
        std::string entry_name = entry.path().filename().string();
        if (entry_name.size() >= prefix.size() + suffix.size() &&
            entry_name.starts_with(prefix) && entry_name.ends_with(suffix)) {
            results.push_back(entry.path().string());
        }
    }
    return results;
}

// ---------------------------------------------------------------------------
// TOML value helpers
// ---------------------------------------------------------------------------
static std::string read_string(const toml::table& tbl, const char* key,
                               const std::string& default_val = "") {
    auto node = tbl.get(key);
    if (node && node->is_string())
        return std::string{node->value<std::string>().value_or(default_val)};
    return default_val;
}

static uint32_t
read_uint32(const toml::table& tbl, const char* key, uint32_t default_val = 0) {
    auto node = tbl.get(key);
    if (node && node->is_integer()) {
        auto val = node->value<int64_t>();
        if (val && *val >= 0)
            return static_cast<uint32_t>(*val);
    }
    return default_val;
}

static bool
read_bool(const toml::table& tbl, const char* key, bool default_val = false) {
    auto node = tbl.get(key);
    if (node && node->is_boolean())
        return node->value<bool>().value_or(default_val);
    return default_val;
}

static double
read_double(const toml::table& tbl, const char* key, double default_val = 0.0) {
    auto node = tbl.get(key);
    if (node && node->is_floating_point()) {
        return node->value<double>().value_or(default_val);
    }
    if (node && node->is_integer()) {
        auto val = node->value<int64_t>();
        if (val)
            return static_cast<double>(*val);
    }
    return default_val;
}

// ---------------------------------------------------------------------------
// Parse [[dispatcher]]
// ---------------------------------------------------------------------------
static DispatcherDef parse_dispatcher(const toml::table& tbl) {
    DispatcherDef def;
    def.name = read_string(tbl, "name");
    def.threads = static_cast<uint16_t>(read_uint32(tbl, "threads", 1));

    if (auto* arr = tbl.get("cpu_affinity")) {
        if (arr->is_array()) {
            for (const auto& v : *arr->as_array()) {
                if (v.is_integer()) {
                    auto val = v.value<int64_t>();
                    if (val && *val >= 0 && *val <= 255)
                        def.cpu_affinity.push_back(static_cast<uint8_t>(*val));
                }
            }
        }
    }
    return def;
}

// ---------------------------------------------------------------------------
// Parse a [[actor]] table from TOML
// ---------------------------------------------------------------------------
static ActorDef parse_actor(const toml::table& tbl) {
    ActorDef def;
    def.id = read_string(tbl, "id");
    def.behavior = read_string(tbl, "behavior");
    def.supervisor = read_string(tbl, "supervisor");
    def.dispatcher = read_string(tbl, "dispatcher");
    def.mailbox_capacity = read_uint32(tbl, "mailbox_capacity");
    def.dispatch_policy =
        parse_dispatch_policy(read_string(tbl, "dispatch_policy", "Cooperative"));

    if (auto* res = tbl.get("resources")) {
        if (res->is_table()) {
            auto& rt = *res->as_table();
            def.resources.slab_class_bytes = read_uint32(rt, "slab_class_"
                                                             "bytes");
            def.resources.max_memory_kb = read_uint32(rt, "max_memory_kb");
        }
    }

    if (auto* mb = tbl.get("mailbox")) {
        if (mb->is_table()) {
            auto& mt = *mb->as_table();
            def.mailbox.policy =
                parse_overflow_policy(read_string(mt, "policy", "reject_newest"));
            def.mailbox.priority_aware = read_bool(mt, "priority_aware", false);
            def.mailbox.max_overflow_depth =
                read_uint32(mt, "max_overflow_depth", 0);
        }
    }

    if (auto* a = tbl.get("args")) {
        if (a->is_table()) {
            for (const auto& [k, v] : *a->as_table()) {
                if (v.is_string())
                    def.args[std::string{k.str()}] =
                        std::string{v.value<std::string>().value_or("")};
                else if (v.is_integer())
                    def.args[std::string{k.str()}] =
                        std::to_string(v.value<int64_t>().value_or(0));
                else if (v.is_floating_point())
                    def.args[std::string{k.str()}] =
                        std::to_string(v.value<double>().value_or(0.0));
                else if (v.is_boolean())
                    def.args[std::string{k.str()}] =
                        v.value<bool>().value_or(false) ? "true" : "false";
            }
        }
    }

    return def;
}

// ---------------------------------------------------------------------------
// Raw actor capture (includes inherits for template resolution)
// ---------------------------------------------------------------------------
struct RawActor {
    ActorDef def;
    std::string inherits;
};

// ---------------------------------------------------------------------------
// Parse [[actor]] from a toml table into RawActor (capturing inherits)
// ---------------------------------------------------------------------------
static RawActor parse_raw_actor(const toml::table& tbl) {
    RawActor raw;
    raw.def = parse_actor(tbl);
    raw.inherits = read_string(tbl, "inherits");
    return raw;
}

// ---------------------------------------------------------------------------
// Parse a TOML file into raw data
// ---------------------------------------------------------------------------
struct FileData {
    SystemDef system;
    std::vector<DispatcherDef> dispatchers;
    std::vector<RawActor> actors;
    std::unordered_map<std::string, ActorDef> templates;
};

static result<FileData>
parse_file_data(const std::string& filepath, bool is_entrypoint) {
    FileData data;

    toml::table root;
    try {
        root = toml::parse_file(filepath);
    } catch (const toml::parse_error&) {
        error err(errors::unknown);
        HPACTOR_LOG_ERROR(log::LogCategory::kConfig, ActorId{0}, 0,
                          "topology parse error",
                          log::field_lit("error", err.message().c_str()));
        return result<FileData>::make(std::move(err));
    }

    // Validate: imported files must not have [system] or imports
    if (!is_entrypoint && root.contains("system")) {
        error err(errors::unknown);
        return result<FileData>::make(std::move(err));
    }

    // Parse [system] (entrypoint only)
    if (is_entrypoint) {
        auto* sys_tbl = root.get("system");
        if (!sys_tbl || !sys_tbl->is_table()) {
            error err(errors::unknown);
            return result<FileData>::make(std::move(err));
        }
        auto& st = *sys_tbl->as_table();
        data.system.version = read_string(st, "version", "1.0");
        data.system.scheduler_threads = read_uint32(st, "scheduler_threads", 4);
        data.system.max_queue_depth = read_uint32(st, "max_queue_depth", 1024);
        data.system.default_mailbox_size =
            read_uint32(st, "default_mailbox_size", 1024);
        data.system.enable_network = read_bool(st, "enable_network");
        data.system.tcp_port = static_cast<uint16_t>(read_uint32(st, "tcp_"
                                                                     "port"));
        data.system.spawn_timeout_ms = read_uint32(st, "spawn_timeout_ms", 5000);
        data.system.enable_http_gateway = read_bool(st, "enable_http_gateway");
        data.system.http_bind_host = read_string(st, "http_bind_host", "0.0.0.0");
        data.system.http_port =
            static_cast<uint16_t>(read_uint32(st, "http_port", 8080));
        data.system.http_max_connections =
            read_uint32(st, "http_max_connections", 1000);
        data.system.http_max_request_size =
            read_uint32(st, "http_max_request_size", 1048576);
        data.system.http_reply_timeout_ms =
            read_uint32(st, "http_reply_timeout_ms", 5000);
        data.system.use_coroutines = read_bool(st, "use_coroutines");

        // Metrics subsystem
        if (auto* metrics_node = st.get("metrics")) {
            if (metrics_node->is_table()) {
                auto& mt = *metrics_node->as_table();
                data.system.metrics_enabled = read_bool(mt, "enabled", true);
                data.system.metrics_ring_buffer_capacity =
                    read_uint32(mt, "ring_buffer_capacity", 65536);
                data.system.metrics_path =
                    read_string(mt, "metrics_path", "/metrics");
            }
        }

        // Logging subsystem
        if (auto* log_node = st.get("logging")) {
            if (log_node->is_table()) {
                auto& lt = *log_node->as_table();
                data.system.logging.enabled = read_bool(lt, "enabled", true);

                // default_level (string → LogLevel)
                std::string lvl_str = read_string(lt, "default_level", "info");
                if (auto parsed = log::parse_level(lvl_str); parsed.has_value())
                    data.system.logging.default_level = parsed.value();

                // format (string → LogFormat)
                std::string fmt_str = read_string(lt, "format", "json");
                if (fmt_str == "text")
                    data.system.logging.format = log::LogFormat::kText;
                else
                    data.system.logging.format = log::LogFormat::kJson;

                data.system.logging.ring_buffer_capacity =
                    read_uint32(lt, "ring_buffer_capacity", 65536);

                // flush_on_level (string → LogLevel)
                std::string flush_str = read_string(lt, "flush_on_level", "error");
                if (auto parsed = log::parse_level(flush_str); parsed.has_value())
                    data.system.logging.flush_on_level = parsed.value();

                data.system.logging.file_path = read_string(lt, "file_path", "");

                // drop_policy (string → DropPolicy)
                std::string drop_str =
                    read_string(lt, "drop_policy", "drop_newest");
                if (drop_str == "drop_newest")
                    data.system.logging.drop_policy = log::DropPolicy::kDropNewest;

                // sinks (array of strings → vector<LogSinkKind>)
                if (auto* sinks_arr = lt.get("sinks")) {
                    if (sinks_arr->is_array()) {
                        for (const auto& v : *sinks_arr->as_array()) {
                            if (v.is_string()) {
                                std::string s = std::string{
                                    v.value<std::string>().value_or("")};
                                if (s == "stderr")
                                    data.system.logging.sinks.push_back(
                                        log::LogSinkKind::kStderr);
                                else if (s == "file")
                                    data.system.logging.sinks.push_back(
                                        log::LogSinkKind::kFile);
                                else if (s == "rotating_file")
                                    data.system.logging.sinks.push_back(
                                        log::LogSinkKind::kRotatingFile);
                            }
                        }
                    }
                }

                // [system.logging.levels] sub-table
                if (auto* levels_node = lt.get("levels")) {
                    if (levels_node->is_table()) {
                        for (const auto& [key, val] : *levels_node->as_table()) {
                            if (!val.is_string())
                                continue;
                            auto cat =
                                log::parse_category(std::string_view{key.str()});
                            if (!cat.has_value())
                                continue;
                            auto lvl = log::parse_level(std::string_view{
                                val.value<std::string>().value_or("")});
                            if (!lvl.has_value())
                                continue;
                            auto idx = static_cast<size_t>(cat.value());
                            data.system.logging.levels[idx] = lvl.value();
                        }
                    }
                }

                // [system.logging.rotating_file] sub-table
                if (auto* rf_node = lt.get("rotating_file")) {
                    if (rf_node->is_table()) {
                        auto& rft = *rf_node->as_table();
                        data.system.logging.rotating_file.path =
                            read_string(rft, "path", "");
                        data.system.logging.rotating_file.max_bytes =
                            static_cast<uint64_t>(
                                read_uint32(rft, "max_bytes", 104857600));
                        data.system.logging.rotating_file.max_files =
                            read_uint32(rft, "max_files", 5);
                    }
                }
            }
        }

        // CLI subsystem
        if (auto* cli_node = st.get("cli")) {
            if (cli_node->is_table()) {
                auto& ct = *cli_node->as_table();
                data.system.cli.enabled = read_bool(ct, "enabled", true);
                data.system.cli.listen_path = read_string(ct, "listen_path", "");
                data.system.cli.tcp_port =
                    static_cast<uint16_t>(read_uint32(ct, "tcp_port", 0));
                data.system.cli.default_format =
                    read_string(ct, "default_format", "pretty");
                data.system.cli.page_size = read_uint32(ct, "page_size", 50);
            }
        }

        // Mailbox subsystem [system.mailbox]
        if (auto* mb_node = st.get("mailbox")) {
            if (mb_node->is_table()) {
                auto& mt = *mb_node->as_table();
                data.system.mailbox.default_capacity =
                    read_uint32(mt, "default_capacity", 1024);
                data.system.mailbox.default_byte_capacity =
                    read_uint32(mt, "default_byte_capacity", 0);
                data.system.mailbox.default_policy = parse_overflow_policy(
                    read_string(mt, "default_policy", "reject_newest"));
                data.system.mailbox.high_watermark =
                    read_double(mt, "high_watermark", 0.80);
                data.system.mailbox.low_watermark =
                    read_double(mt, "low_watermark", 0.50);
                data.system.mailbox.protected_system_messages =
                    read_uint32(mt, "protected_system_messages", 32);
                data.system.mailbox.backpressure = parse_backpressure_mode(
                    read_string(mt, "backpressure", "local_and_remote"));
            }
        }

        // Dead letters subsystem [system.dead_letters]
        if (auto* dl_node = st.get("dead_letters")) {
            if (dl_node->is_table()) {
                auto& dt = *dl_node->as_table();
                data.system.dead_letters.enabled = read_bool(dt, "enabled", true);
                data.system.dead_letters.capacity =
                    read_uint32(dt, "capacity", 4096);
                data.system.dead_letters.byte_capacity =
                    read_uint32(dt, "byte_capacity", 0);
                data.system.dead_letters.max_payload_sample_bytes =
                    read_uint32(dt, "max_payload_sample_bytes", 512);
                data.system.dead_letters.overflow_policy = parse_dl_overflow_policy(
                    read_string(dt, "overflow_policy", "drop_oldest_record"));
                data.system.dead_letters.store_payload =
                    read_bool(dt, "store_payload", true);
                data.system.dead_letters.alert_on_first_failure =
                    read_bool(dt, "alert_on_first_failure", false);
                data.system.dead_letters.alert_threshold_per_minute =
                    read_uint32(dt, "alert_threshold_per_minute", 100);
            }
        }

        // ── Service discovery ────────────────────────────────────
        if (auto dt = st["discovery"]; dt.is_table()) {
            data.system.discovery_backend =
                read_string(*dt.as_table(), "backend", "");
        }

        if (auto* imp_arr = st.get("imports")) {
            if (imp_arr->is_array()) {
                for (const auto& v : *imp_arr->as_array()) {
                    if (v.is_string())
                        data.system.imports.push_back(
                            std::string{v.value<std::string>().value_or("")});
                }
            }
        }
    }

    // Parse [[dispatcher]]
    if (auto* disp_arr = root.get("dispatcher")) {
        if (disp_arr->is_array()) {
            for (const auto& elem : *disp_arr->as_array()) {
                if (elem.is_table())
                    data.dispatchers.push_back(parse_dispatcher(*elem.as_table()));
            }
        }
    }

    // Parse [template.*]
    if (auto* tmpl = root.get("template")) {
        if (tmpl->is_table()) {
            for (const auto& [name, tbl] : *tmpl->as_table()) {
                if (tbl.is_table())
                    data.templates[std::string{name.str()}] =
                        parse_actor(*tbl.as_table());
            }
        }
    }

    // Parse [[actor]]
    if (auto* act_arr = root.get("actor")) {
        if (act_arr->is_array()) {
            for (const auto& elem : *act_arr->as_array()) {
                if (elem.is_table())
                    data.actors.push_back(parse_raw_actor(*elem.as_table()));
            }
        }
    }

    return result<FileData>::make(std::move(data));
}

// ---------------------------------------------------------------------------
// Deep merge overrides into base
// ---------------------------------------------------------------------------
static void deep_merge(ActorDef& base, const ActorDef& overrides) {
    if (!overrides.behavior.empty())
        base.behavior = overrides.behavior;
    if (!overrides.dispatcher.empty())
        base.dispatcher = overrides.dispatcher;
    if (!overrides.supervisor.empty())
        base.supervisor = overrides.supervisor;
    if (overrides.mailbox_capacity != 0)
        base.mailbox_capacity = overrides.mailbox_capacity;
    base.dispatch_policy = overrides.dispatch_policy;

    if (overrides.resources.slab_class_bytes != 0)
        base.resources.slab_class_bytes = overrides.resources.slab_class_bytes;
    if (overrides.resources.max_memory_kb != 0)
        base.resources.max_memory_kb = overrides.resources.max_memory_kb;

    if (overrides.mailbox.policy != hpactor::mailbox::OverflowPolicy::RejectNewest)
        base.mailbox.policy = overrides.mailbox.policy;
    base.mailbox.priority_aware = overrides.mailbox.priority_aware;
    if (overrides.mailbox.max_overflow_depth != 0)
        base.mailbox.max_overflow_depth = overrides.mailbox.max_overflow_depth;

    for (const auto& [k, v] : overrides.args) {
        base.args[k] = v;
    }
}

// ---------------------------------------------------------------------------
// Resolve template inheritance
// ---------------------------------------------------------------------------
static result<std::vector<ActorDef>>
resolve_templates(const std::vector<RawActor>& raw_actors,
                  const std::unordered_map<std::string, ActorDef>& templates) {
    std::vector<ActorDef> resolved;
    resolved.reserve(raw_actors.size());

    for (const auto& raw : raw_actors) {
        if (raw.inherits.empty()) {
            resolved.push_back(raw.def);
            continue;
        }

        auto tmpl_it = templates.find(raw.inherits);
        if (tmpl_it == templates.end()) {
            error err(errors::unknown);
            return result<std::vector<ActorDef>>::make(std::move(err));
        }

        ActorDef merged = tmpl_it->second;
        deep_merge(merged, raw.def);
        merged.id = raw.def.id;
        resolved.push_back(std::move(merged));
    }

    return result<std::vector<ActorDef>>::make(std::move(resolved));
}

// ---------------------------------------------------------------------------
// Validation
// ---------------------------------------------------------------------------
static bool validate(const TopologyModel& model, std::string& error_msg) {
    // Duplicate/empty actor ids
    std::unordered_set<std::string> ids;
    for (const auto& actor : model.actors) {
        if (actor.id.empty()) {
            error_msg = "actor has empty id";
            return false;
        }
        if (ids.find(actor.id) != ids.end()) {
            error_msg = "duplicate actor id '" + actor.id + "'";
            return false;
        }
        ids.insert(actor.id);
        if (actor.behavior.empty()) {
            error_msg = "actor '" + actor.id + "' has no behavior";
            return false;
        }
    }

    // Dispatcher references
    std::unordered_set<std::string> disp_names;
    for (const auto& d : model.dispatchers)
        disp_names.insert(d.name);

    for (const auto& actor : model.actors) {
        if (!actor.dispatcher.empty() &&
            disp_names.find(actor.dispatcher) == disp_names.end()) {
            error_msg = "actor '" + actor.id + "' references unknown dispatcher '" +
                        actor.dispatcher + "'";
            return false;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------
// Topological sort (Kahn's algorithm)
// ---------------------------------------------------------------------------
static result<std::vector<ActorDef>>
topological_sort(std::vector<ActorDef> actors) {
    std::unordered_map<std::string, size_t> id_to_idx;
    for (size_t i = 0; i < actors.size(); ++i)
        id_to_idx[actors[i].id] = i;

    std::unordered_map<std::string, std::vector<size_t>> children;
    std::vector<size_t> in_degree(actors.size(), 0);

    for (size_t i = 0; i < actors.size(); ++i) {
        const auto& sup = actors[i].supervisor;
        if (!sup.empty()) {
            auto it = id_to_idx.find(sup);
            if (it == id_to_idx.end()) {
                error err(errors::unknown);
                return result<std::vector<ActorDef>>::make(std::move(err));
            }
            children[sup].push_back(i);
            in_degree[i]++;
        }
    }

    std::deque<size_t> queue;
    for (size_t i = 0; i < actors.size(); ++i)
        if (in_degree[i] == 0)
            queue.push_back(i);

    std::vector<ActorDef> sorted;
    sorted.reserve(actors.size());

    while (!queue.empty()) {
        size_t current = queue.front();
        queue.pop_front();
        sorted.push_back(std::move(actors[current]));

        auto child_it = children.find(sorted.back().id);
        if (child_it != children.end()) {
            for (size_t child_idx : child_it->second) {
                if (--in_degree[child_idx] == 0)
                    queue.push_back(child_idx);
            }
        }
    }

    if (sorted.size() != actors.size()) {
        error err(errors::unknown);
        return result<std::vector<ActorDef>>::make(std::move(err));
    }

    return result<std::vector<ActorDef>>::make(std::move(sorted));
}

} // anonymous namespace

// =============================================================================
// TomlParser::parse
// =============================================================================
result<TopologyModel> TomlParser::parse(const std::string& entrypoint_path) {
    fs::path entry_fs(entrypoint_path);
    fs::path base_dir = entry_fs.parent_path();
    if (base_dir.empty())
        base_dir = ".";

    // Phase 1: Parse entrypoint
    auto entry_parse = parse_file_data(entrypoint_path, true);
    if (!entry_parse.has_value()) {
        return result<TopologyModel>::make(entry_parse.error());
    }
    FileData entry_data = std::move(entry_parse.value());

    // Phase 2: Collect imports' data
    std::vector<FileData> imported_data;
    for (const auto& import_pattern : entry_data.system.imports) {
        fs::path import_path = base_dir / import_pattern;
        auto files = expand_glob(import_path.string());
        for (const auto& file : files) {
            auto fc = parse_file_data(file, false);
            if (!fc.has_value())
                return result<TopologyModel>::make(fc.error());
            imported_data.push_back(std::move(fc.value()));
        }
    }

    // Phase 3: Merge dispatchers (imported first, then entrypoint)
    std::vector<DispatcherDef> all_dispatchers;
    std::unordered_map<std::string, ActorDef> all_templates;
    std::vector<RawActor> all_raw_actors;

    for (auto& imp : imported_data) {
        for (auto& d : imp.dispatchers)
            all_dispatchers.push_back(std::move(d));
        for (auto& a : imp.actors)
            all_raw_actors.push_back(std::move(a));
        // Templates: first-wins (imported files are processed first)
        for (auto& [name, tmpl] : imp.templates) {
            if (all_templates.find(name) == all_templates.end())
                all_templates[name] = std::move(tmpl);
        }
    }

    // Entrypoint dispatchers, templates, actors (last, can override templates)
    for (auto& d : entry_data.dispatchers)
        all_dispatchers.push_back(std::move(d));
    for (auto& a : entry_data.actors)
        all_raw_actors.push_back(std::move(a));
    for (auto& [name, tmpl] : entry_data.templates) {
        if (all_templates.find(name) == all_templates.end())
            all_templates[name] = std::move(tmpl);
    }

    // Phase 4: Resolve template inheritance
    auto resolved_result = resolve_templates(all_raw_actors, all_templates);
    if (!resolved_result.has_value())
        return result<TopologyModel>::make(resolved_result.error());

    TopologyModel model;
    model.system = std::move(entry_data.system);
    model.dispatchers = std::move(all_dispatchers);
    model.actors = std::move(resolved_result.value());

    // Phase 5: Validate
    std::string error_msg;
    if (!validate(model, error_msg)) {
        error err(errors::unknown);
        return result<TopologyModel>::make(std::move(err));
    }

    // Phase 6: Topological sort
    auto sorted_result = topological_sort(std::move(model.actors));
    if (!sorted_result.has_value())
        return result<TopologyModel>::make(sorted_result.error());
    model.actors = std::move(sorted_result.value());

    HPACTOR_LOG_INFO(log::LogCategory::kConfig, ActorId{0}, 0, "topology loaded",
                     log::field_lit("path", entrypoint_path.c_str()));

    return result<TopologyModel>::make(std::move(model));
}

} // namespace hpactor::config
