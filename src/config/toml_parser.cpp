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
#include <hpactor/config/toml_file_data.hpp>
#include <hpactor/config/toml_parse_context.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/config/toml_parser_registry.hpp>
#include <hpactor/config/toml_table_view.hpp>
#include <hpactor/log/log_category.hpp>
#include <hpactor/log/logger.hpp>

#include <toml.hpp>

#include <deque>
#include <filesystem>
#include <unordered_set>

namespace fs = std::filesystem;

namespace hpactor::config {

namespace {

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
// Parse a single TOML file using the registered parser pipeline
// ---------------------------------------------------------------------------
static result<TomlFileData>
parse_file_data(const std::string& filepath, bool is_entrypoint) {
    TomlFileData data;

    toml::table root;
    try {
        root = toml::parse_file(filepath);
    } catch (const toml::parse_error&) {
        error err(errors::unknown);
        HPACTOR_LOG_ERROR(log::LogCategory::kConfig, ActorId{0}, 0,
                          "topology parse error",
                          log::field_lit("error", err.message().c_str()));
        return result<TomlFileData>::make(std::move(err));
    }

    TomlParseContext ctx(filepath, is_entrypoint);
    TomlTableView root_view = make_toml_table_view(&root);

    // Imported files must not contain [system]
    if (!is_entrypoint && root_view.contains("system")) {
        error err(errors::unknown);
        return result<TomlFileData>::make(std::move(err));
    }

    // Entrypoint must have [system]
    if (is_entrypoint) {
        auto system_view = root_view.table("system");
        if (!system_view.valid()) {
            error err(errors::unknown);
            return result<TomlFileData>::make(std::move(err));
        }
    }

    // Snapshot registered parsers before use
    auto document_parsers =
        TomlParserRegistry::instance().create_document_parsers();
    auto system_parsers = TomlParserRegistry::instance().create_system_parsers();

    if (document_parsers.empty() || (is_entrypoint && system_parsers.empty())) {
        error err(errors::unknown);
        HPACTOR_LOG_ERROR(log::LogCategory::kConfig, ActorId{0}, 0,
                          "no registered TOML parsers found");
        return result<TomlFileData>::make(std::move(err));
    }

    // Run document parsers on the root table
    for (const auto& parser : document_parsers) {
        auto parsed = parser->parse(root_view, data, ctx);
        if (!parsed.has_value())
            return result<TomlFileData>::make(parsed.error());
    }

    // Run system parsers on [system] table (entrypoint only)
    if (is_entrypoint) {
        auto system_view = root_view.table("system");
        for (const auto& parser : system_parsers) {
            auto parsed = parser->parse(system_view, data.system, ctx);
            if (!parsed.has_value())
                return result<TomlFileData>::make(parsed.error());
        }
    }

    return result<TomlFileData>::make(std::move(data));
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
resolve_templates(const std::vector<TomlRawActor>& raw_actors,
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
    TomlFileData entry_data = std::move(entry_parse.value());

    // Phase 2: Collect imports' data
    std::vector<TomlFileData> imported_data;
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
    std::vector<TomlRawActor> all_raw_actors;

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
