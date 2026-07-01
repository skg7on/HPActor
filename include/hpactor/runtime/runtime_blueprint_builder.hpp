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

#include <hpactor/runtime/runtime_blueprint.hpp>

#include <hpactor/config/reload_report.hpp>
#include <hpactor/types/types.hpp>

#include <string>

namespace hpactor {

struct Config;

/// \brief Builds immutable \c RuntimeBlueprint from user-provided input.
///
/// Supports \c from_config(config), \c from_config_and_topology(config, path),
/// and \c from_topology(path). All parsing and validation happens before the
/// blueprint is returned — no runtime side effects.
///
/// Public headers do not expose \c toml++.
class RuntimeBlueprintBuilder final {
  public:
    /// \brief Build a blueprint from a \c Config only (no TOML topology).
    ///
    /// Validates endpoint, ports, bounds, feature dependencies, and
    /// process constraints before returning. No threads, listeners, actor
    /// spawns, or daemonization occur.
    static result<RuntimeBlueprint> from_config(const Config& config) noexcept;

    /// \brief Compute a reload classification diff between two blueprints.
    ///
    /// Compares fingerprints. If they match, the report has fully_applied=true
    /// and zero fields. If they differ, fields are classified as Live,
    /// RestartRequired, or Immutable based on registered descriptors.
    static ReloadReport diff(const RuntimeBlueprint& current,
                             const RuntimeBlueprint& candidate) noexcept;
};

} // namespace hpactor
