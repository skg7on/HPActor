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

#include <hpactor/config/topology_model.hpp>
#include <hpactor/types/types.hpp>

#include <string>

namespace hpactor::config {

// -----------------------------------------------------------------------------
// TomlParser — parses TOML entrypoint files into validated TopologyModel
//
// Handles import resolution, file merging, template inheritance (deep merge),
// validation, and topological sort of the actor dependency DAG.
// -----------------------------------------------------------------------------
class TomlParser {
  public:
    // Parse a TOML entrypoint file into a validated, topologically sorted
    // TopologyModel. Returns error on parse failure, validation failure,
    // or circular dependency.
    static result<TopologyModel> parse(const std::string& entrypoint_path);
};

} // namespace hpactor::config
