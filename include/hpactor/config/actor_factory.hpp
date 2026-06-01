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

#include <functional>
#include <memory>

namespace hpactor {

class AbstractActor;
class ActorContext;
class ActorSystem;

namespace config {

/// \brief Type-erased factory function for constructing actors.
///
/// Follows the same signature convention as ActorSystem::spawn<T>(): the
/// ActorContext* may be nullptr during construction and is set later via
/// set_context() during spawn_configured().
///
/// \param[in] ctx Actor context pointer (may be nullptr during construction).
/// \param[in] sys Reference to the actor system.
/// \return Shared pointer to the constructed actor.
using ActorFactory =
    std::function<std::shared_ptr<AbstractActor>(ActorContext*, ActorSystem&)>;

} // namespace config
} // namespace hpactor
