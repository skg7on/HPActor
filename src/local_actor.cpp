#include <hpactor/actor/local_actor.hpp>

namespace hpactor {

LocalActor::LocalActor(ActorContext* ctx, ActorSystem& sys)
    : AbstractActor(ActorId{}, 0, sys), ctx_(ctx) {}

LocalActor::LocalActor(ActorId id, ActorContext* ctx, ActorSystem& sys)
    : AbstractActor(id, 0, sys), ctx_(ctx) {}

} // namespace hpactor
