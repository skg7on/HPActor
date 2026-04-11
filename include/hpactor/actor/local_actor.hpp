#pragma once

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor_context.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// local_actor - base class for actors with access to ActorContext
// -----------------------------------------------------------------------------
class local_actor : public abstract_actor {
  public:
    ActorContext* context() {
        return ctx_;
    }
    ActorSystem& home_system() {
        return system();
    }

  protected:
    local_actor(ActorContext* ctx, ActorSystem& sys);
    local_actor(ActorId id, ActorContext* ctx, ActorSystem& sys);

    virtual void on_activate() {}
    virtual void on_deactivate() {}

  private:
    ActorContext* ctx_ = nullptr;
};

} // namespace hpactor