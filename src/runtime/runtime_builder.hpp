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

#include "runtime_blueprint.hpp"

#include <hpactor/types/types.hpp>

#include <memory>

namespace hpactor {

class ActorSystem;

/// \brief Result of a successful \c RuntimeBuilder::build().
///
/// Owns the complete, stopped component graph. Nothing has been started —
/// no threads, listeners, timers, or actor spawns. The facade is valid
/// but not ready.
struct RuntimeBuildResult final {
    /// \brief The constructed but not-yet-started ActorSystem.
    /// All components exist, but no threads, listeners, or timers are active.
    std::unique_ptr<ActorSystem> system;
};

/// \brief Constructs the complete component graph from an immutable
///        \c RuntimeBlueprint without starting any component.
///
/// This is the composition root: it allocates every runtime component
/// in dependency order but does not call \c start() on any of them.
/// The caller (typically \c RuntimeCoordinator) owns startup ordering.
class RuntimeBuilder final {
  public:
    /// \brief Build the complete, stopped component graph.
    ///
    /// Validates that all dependencies are satisfiable, constructs
    /// every component, wires ports, and returns the graph. No threads,
    /// listeners, timers, or actor spawns occur.
    ///
    /// The returned \c ActorSystem is valid but not ready — the
    /// coordinator must call start stages before it accepts work.
    static result<RuntimeBuildResult> build(RuntimeBlueprint bp) noexcept;
};

} // namespace hpactor
