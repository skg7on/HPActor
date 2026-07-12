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

#include <hpactor/runtime/configured_actor_provider.hpp>

namespace hpactor {

class ActorSystem;

/// \brief Create a \c ConfiguredActorProviderPort that resolves C++ actors
///        through the \c ActorFactoryRegistry singleton and spawns them via
///        \c ActorSystem::spawn_configured().
///
/// The returned port never throws; all function pointers are noexcept.
/// The \c context pointer holds the \c ActorSystem* and is owned by the
/// caller (typically \c PythonNativeSystem).
///
/// \param[in] system  The actor system used for spawn_configured().
/// \return A configured provider port for BuiltinCpp plans.
[[nodiscard]] ConfiguredActorProviderPort
make_builtin_cpp_provider_port(ActorSystem& system) noexcept;

} // namespace hpactor
