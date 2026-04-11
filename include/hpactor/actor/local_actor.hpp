#pragma once

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor_context.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// LocalActor - base class for actors with access to ActorContext
// -----------------------------------------------------------------------------
class LocalActor : public AbstractActor {
public:
    ActorContext* context() { return ctx_; }
    ActorSystem& home_system() { return system(); }

protected:
    LocalActor(ActorContext* ctx, ActorSystem& sys);
    LocalActor(ActorId id, ActorContext* ctx, ActorSystem& sys);

    virtual void on_activate() {}
    virtual void on_deactivate() {}

private:
    ActorContext* ctx_ = nullptr;
};

} // namespace hpactor