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

#include <hpactor/types/types.hpp>

namespace hpactor {

/// \brief Well-known system actor identifiers.
///
/// System actors occupy the reserved ID range \c 0xFFFF0000 – \c 0xFFFFFFFF.
/// These are pre-assigned so that remote nodes and subsystems can address
/// them without discovery.

/// \brief Actor ID of the \c SpawnReceiver system actor.
///
/// Handles remote spawn requests and routes \c SpawnRequest / \c SpawnResponse
/// messages.
constexpr ActorId SpawnReceiverId = ActorId(0xFFFF0001);

/// \brief Type tag used in \c ActorAddress for system actors.
constexpr ActorType SystemActorType = 0xFFFF0000;

} // namespace hpactor
