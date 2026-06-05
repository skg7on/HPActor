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

#include <memory>

namespace hpactor {

class ActorSystem;

/// \brief Coordinates system-wide backpressure signal emission and handling.
///
/// Extracted from \c ActorSystem to isolate backpressure logic. Currently
/// a PIMPL stub wired to the \c ActorSystem backpressure methods. Future
/// iterations will move signal routing, pressure-state aggregation, and
/// remote signal serialization into this coordinator.
///
/// \note Thread safety: Internally synchronized via PIMPL indirection.
///       The \c ActorSystem reference must outlive this coordinator.
class BackpressureCoordinator {
  public:
    /// \brief Construct and wire into a running actor system.
    ///
    /// \param[in] system The owning \c ActorSystem whose backpressure
    ///                   methods this coordinator will orchestrate.
    explicit BackpressureCoordinator(ActorSystem& system);

    ~BackpressureCoordinator();

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hpactor
