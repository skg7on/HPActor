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

#include <hpactor/python/python_topology_preparer.hpp>

#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/toml_parser.hpp>
#include <hpactor/config/topology_model.hpp>
#include <hpactor/python/python_topology_types.hpp>

#include <cstring>
#include <unordered_map>

namespace hpactor::python {
namespace {

// FNV-1a hash helpers (consistent with runtime_blueprint_builder.cpp).
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
constexpr uint64_t kFnvPrime = 0x100000001b3ULL;

uint64_t hash_bytes(const uint8_t* data, size_t len, uint64_t seed) noexcept {
    uint64_t h = seed;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint64_t>(data[i]);
        h *= kFnvPrime;
    }
    return h;
}

uint64_t hash_u64(uint64_t v, uint64_t seed) noexcept {
    return hash_bytes(reinterpret_cast<const uint8_t*>(&v), sizeof(v), seed);
}

uint64_t hash_string(std::string_view s, uint64_t seed) noexcept {
    return hash_bytes(reinterpret_cast<const uint8_t*>(s.data()), s.size(), seed);
}

/// \brief Compute a deterministic fingerprint of the topology structure.
uint64_t compute_topology_fingerprint(
    const config::TopologyModel& model,
    const std::vector<PreparedActorSpec>& specs) noexcept {
    uint64_t fp = kFnvOffsetBasis;
    fp = hash_u64(specs.size(), fp);
    for (const auto& spec : specs) {
        const auto& def = model.actors[spec.topology_index];
        fp = hash_string(def.id, fp);
        fp = hash_string(def.behavior, fp);
        fp = hash_u64(spec.args_fingerprint, fp);
    }
    return fp;
}

} // namespace

// ── ParsedTopologyPlan ──────────────────────────────────────────────────────

const config::TopologyModel& ParsedTopologyPlan::model() const noexcept {
    return *model_;
}

std::span<const PreparedActorSpec> ParsedTopologyPlan::actors() const noexcept {
    return specs_;
}

uint64_t ParsedTopologyPlan::topology_fingerprint() const noexcept {
    return topology_fingerprint_;
}

result<std::unique_ptr<PreparedTopology>>
ParsedTopologyPlan::bind_manifest(std::span<const FactoryTokenBinding> bindings,
                                  uint64_t policy_fingerprint) const noexcept {
    // Build a lookup from topology_index to binding for Python actors.
    std::unordered_map<size_t, const FactoryTokenBinding*> binding_map;
    for (const auto& b : bindings) {
        if (b.factory_token == 0) {
            return result<std::unique_ptr<PreparedTopology>>::make(
                error(errors::invalid_argument,
                      "factory_token must be non-zero"));
        }
        binding_map[b.topology_index] = &b;
    }

    // Verify every Python actor has a matching binding.
    auto prepared = std::unique_ptr<PreparedTopology>(new PreparedTopology());

    // Deep-copy the model.
    prepared->model_ =
        std::make_unique<config::TopologyModel>(*model_);
    prepared->specs_ = specs_;

    for (auto& spec : prepared->specs_) {
        if (spec.kind == ConfiguredActorKind::Python) {
            auto it = binding_map.find(spec.topology_index);
            if (it == binding_map.end()) {
                return result<std::unique_ptr<PreparedTopology>>::make(
                    error(errors::invalid_argument,
                          "missing factory token binding for Python actor"));
            }
            if (it->second->args_fingerprint != spec.args_fingerprint) {
                return result<std::unique_ptr<PreparedTopology>>::make(
                    error(errors::invalid_argument,
                          "args_fingerprint mismatch in factory token binding"));
            }
            spec.factory_token = it->second->factory_token;
        }
    }

    // Compute effective fingerprint: topology_fingerprint XOR token hash XOR
    // policy fingerprint.
    uint64_t fp = topology_fingerprint_;
    fp = hash_u64(policy_fingerprint, fp);
    for (const auto& spec : prepared->specs_) {
        fp = hash_u64(spec.factory_token, fp);
    }
    prepared->effective_fingerprint_ = fp;

    return result<std::unique_ptr<PreparedTopology>>::make(
        std::move(prepared));
}

// ── PreparedTopology ────────────────────────────────────────────────────────

const config::TopologyModel& PreparedTopology::model() const noexcept {
    return *model_;
}

std::span<const PreparedActorSpec> PreparedTopology::actors() const noexcept {
    return specs_;
}

uint64_t PreparedTopology::effective_fingerprint() const noexcept {
    return effective_fingerprint_;
}

// ── PythonTopologyPreparer ──────────────────────────────────────────────────

result<std::unique_ptr<ParsedTopologyPlan>>
PythonTopologyPreparer::parse(std::string_view toml_path) noexcept {
    // Phase 1: Parse TOML using the existing parser.
    auto model_result = config::TomlParser::parse(std::string(toml_path));
    if (!model_result.has_value()) {
        return result<std::unique_ptr<ParsedTopologyPlan>>::make(
            model_result.error());
    }

    auto plan = std::unique_ptr<ParsedTopologyPlan>(new ParsedTopologyPlan());
    plan->model_ =
        std::make_unique<config::TopologyModel>(std::move(model_result.value()));

    auto& registry = config::ActorFactoryRegistry::instance();

    // Phase 2: Classify every actor.
    plan->specs_.reserve(plan->model_->actors.size());
    for (size_t i = 0; i < plan->model_->actors.size(); ++i) {
        const auto& def = plan->model_->actors[i];
        PreparedActorSpec spec;
        spec.topology_index = i;

        if (def.behavior.starts_with("python:")) {
            // Parse and validate the Python behavior reference.
            auto ref = parse_python_behavior_ref(def.behavior);
            if (!ref.has_value()) {
                return result<std::unique_ptr<ParsedTopologyPlan>>::make(
                    error(errors::invalid_argument,
                          "invalid Python behavior reference: " +
                          std::string(def.behavior)));
            }

            // Check for collision with a C++ factory of the exact same name.
            if (registry.has(def.behavior)) {
                return result<std::unique_ptr<ParsedTopologyPlan>>::make(
                    error(errors::invalid_argument,
                          "Python behavior '" + def.behavior +
                          "' collides with a C++ factory"));
            }

            spec.kind = ConfiguredActorKind::Python;
            spec.python = std::move(ref.value());
            spec.args_fingerprint = fingerprint_python_actor_args(def);
        } else {
            // C++ actor: validate the behavior name is in the factory registry.
            if (!registry.has(def.behavior)) {
                return result<std::unique_ptr<ParsedTopologyPlan>>::make(
                    error(errors::unknown,
                          "unknown C++ behavior: " + def.behavior));
            }
            spec.kind = ConfiguredActorKind::Cpp;
        }

        plan->specs_.push_back(std::move(spec));
    }

    // Phase 3: Compute topology fingerprint.
    plan->topology_fingerprint_ =
        compute_topology_fingerprint(*plan->model_, plan->specs_);

    return result<std::unique_ptr<ParsedTopologyPlan>>::make(std::move(plan));
}

} // namespace hpactor::python
