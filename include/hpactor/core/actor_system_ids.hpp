#pragma once

#include <hpactor/types.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// Well-known system actor IDs
// Reserved range: 0xFFFF0000 - 0xFFFFFFFF
// -----------------------------------------------------------------------------

constexpr ActorId SpawnReceiverId = ActorId(0xFFFF0001);  // Handles spawn requests

// System actor type (used in ActorAddress for system actors)
constexpr ActorType SystemActorType = 0xFFFF0000;

} // namespace hpactor
