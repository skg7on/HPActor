#include <hpactor/actor/local_actor.hpp>

namespace hpactor {

local_actor::local_actor(ActorContext* ctx, ActorSystem& sys)
    : abstract_actor(ActorId{}, 0, sys), ctx_(ctx) {}

local_actor::local_actor(ActorId id, ActorContext* ctx, ActorSystem& sys)
    : abstract_actor(id, 0, sys), ctx_(ctx) {}

} // namespace hpactor
