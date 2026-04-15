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

// -----------------------------------------------------------------------------
// Well-known system actor IDs
// Reserved range: 0xFFFF0000 - 0xFFFFFFFF
// -----------------------------------------------------------------------------

constexpr ActorId SpawnReceiverId = ActorId(0xFFFF0001);  // Handles spawn requests

// System actor type (used in ActorAddress for system actors)
constexpr ActorType SystemActorType = 0xFFFF0000;

} // namespace hpactor
