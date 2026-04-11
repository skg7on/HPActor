#pragma once

#include "../types_fwd.hpp"

namespace hpactor {

class abstract_actor;
class local_actor;
class event_based_actor;
class blocking_actor;
class scoped_actor;

template <typename... Signatures> class typed_event_based_actor;

template <typename T> class stateful_actor;

class Actor;
class ActorRef;

} // namespace hpactor