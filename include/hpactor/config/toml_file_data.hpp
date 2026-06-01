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

#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::config {

/// \brief Raw actor definition before template resolution.
///
/// If \c inherits is non-empty, the actor inherits fields from the named
/// template during TomlParser import/merge processing.
struct TomlRawActor {
    /// \brief The actor definition fields.
    ActorDef def;
    /// \brief Template name to inherit from (empty = no inheritance).
    std::string inherits;
};

/// \brief Intermediate representation of a single parsed TOML file.
///
/// Holds the raw parsed data before import resolution, template expansion,
/// and cross-file merging.
struct TomlFileData {
    /// \brief System configuration from the file's [system] section (merged
    ///        across imports).
    SystemDef system;
    /// \brief Dispatcher definitions.
    std::vector<DispatcherDef> dispatchers;
    /// \brief Actor definitions with optional template inheritance.
    std::vector<TomlRawActor> actors;
    /// \brief Named actor templates for inheritance.
    std::unordered_map<std::string, ActorDef> templates;
};

} // namespace hpactor::config
