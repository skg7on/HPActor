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

#include <chrono>

namespace hpactor {

/// \brief Options controlling the shutdown sequence.
///
/// Passed to \c ActorSystem::shutdown() and \c ShutdownCoordinator::execute().
/// Each timeout controls how long the corresponding phase waits before the
/// force-stop deadline is reached.
struct ShutdownOptions {
    /// \brief Maximum time for ingress draining.
    std::chrono::milliseconds ingress_timeout{5'000};
    /// \brief Maximum time for actor message draining.
    std::chrono::milliseconds actor_drain_timeout{30'000};
    /// \brief Maximum time for cluster leave handshake.
    std::chrono::milliseconds cluster_leave_timeout{10'000};
    /// \brief Force shutdown after all phase timeouts expire.
    bool force_after_timeout{true};
};

} // namespace hpactor
