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

#pragma once

#include <hpactor/python/python_topology_types.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor::config {
class TopologyModel;
} // namespace hpactor::config

namespace hpactor::python {

/// \brief An immutable parsed topology plan with classified actor specs.
///
/// Produced by PythonTopologyPreparer. Contains the original TopologyModel and
/// a vector of PreparedActorSpec instances classified as Cpp or Python.
class ParsedTopologyPlan final {
  public:
    ParsedTopologyPlan(const ParsedTopologyPlan&) = delete;
    ParsedTopologyPlan& operator=(const ParsedTopologyPlan&) = delete;

    /// \brief Access the underlying topology model.
    [[nodiscard]] const config::TopologyModel& model() const noexcept;

    /// \brief Access the classified actor specs in model order.
    [[nodiscard]] std::span<const PreparedActorSpec> actors() const noexcept;

    /// \brief Deterministic fingerprint of the topology structure.
    ///
    /// Covers ordered actor IDs, behaviors, and sorted argument key/value bytes.
    [[nodiscard]] uint64_t topology_fingerprint() const noexcept;

    /// \brief Bind factory tokens to produce an immutable prepared topology.
    ///
    /// Every Python actor must have exactly one matching FactoryTokenBinding
    /// with a non-zero token and matching args_fingerprint. C++ actors are
    /// unaffected. Returns the PreparedTopology on success.
    ///
    /// \param[in] bindings Factory token bindings for Python actors.
    /// \param[in] policy_fingerprint FNV-1a hash of the application's
    ///            allowed_module_prefixes (0 if no Python actors).
    /// \return A PreparedTopology, or an error.
    [[nodiscard]] result<std::unique_ptr<class PreparedTopology>>
    bind_manifest(std::span<const FactoryTokenBinding> bindings,
                  uint64_t policy_fingerprint) const noexcept;

  private:
    friend class PythonTopologyPreparer;

    ParsedTopologyPlan() = default;

    std::unique_ptr<config::TopologyModel> model_;
    std::vector<PreparedActorSpec> specs_;
    uint64_t topology_fingerprint_{0};
};

/// \brief An immutable topology prepared with bound factory tokens.
///
/// Produced by ParsedTopologyPlan::bind_manifest(). Ready for consumption by
/// TopologyBootstrapTransaction.
class PreparedTopology final {
  public:
    PreparedTopology(const PreparedTopology&) = delete;
    PreparedTopology& operator=(const PreparedTopology&) = delete;

    /// \brief Access the underlying topology model.
    [[nodiscard]] const config::TopologyModel& model() const noexcept;

    /// \brief Access the classified actor specs in model order.
    [[nodiscard]] std::span<const PreparedActorSpec> actors() const noexcept;

    /// \brief Effective fingerprint including topology structure and factory
    ///        bindings.
    [[nodiscard]] uint64_t effective_fingerprint() const noexcept;

  private:
    friend class ParsedTopologyPlan;

    PreparedTopology() = default;

    std::unique_ptr<config::TopologyModel> model_;
    std::vector<PreparedActorSpec> specs_;
    uint64_t effective_fingerprint_{0};
};

/// \brief Side-effect-free topology preparer for Python and C++ actors.
///
/// Owns no runtime state. All methods are pure and deterministic.
class PythonTopologyPreparer final {
  public:
    /// \brief Parse a TOML topology file and classify every actor.
    ///
    /// Calls TomlParser::parse(), then classifies each actor based on its
    /// behavior string: \c "python:" prefix → ConfiguredActorKind::Python,
    /// everything else → ConfiguredActorKind::Cpp.
    ///
    /// Validates Python behavior syntax and checks for C++/Python name
    /// collisions. Does NOT start threads, spawn actors, or mutate runtime
    /// state.
    ///
    /// \param[in] toml_path Path to the topology TOML file.
    /// \return A ParsedTopologyPlan, or an error.
    [[nodiscard]] static result<std::unique_ptr<ParsedTopologyPlan>>
    parse(std::string_view toml_path) noexcept;
};

} // namespace hpactor::python
