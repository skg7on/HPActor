#pragma once

#include "../types/types_fwd.hpp"

namespace hpactor {

class AbstractActor;
class LocalActor;
class EventBasedActor;
class BlockingActor;
class ScopedActor;

template <typename... Signatures> class TypedEventBasedActor;

template <typename T> class StatefulActor;

class Actor;
class ActorRef;

} // namespace hpactor