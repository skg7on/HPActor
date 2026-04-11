#pragma once

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/behavior.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// EventBasedActor - cooperatively scheduled actor with behavior-based
// handling
// -----------------------------------------------------------------------------
class EventBasedActor : public LocalActor {
  public:
    void become(Behavior bh);
    void become_empty();

    void receive(MessageVariant&& msg) override;

  protected:
    virtual Behavior make_behavior() {
        return {};
    }
    void on_activate() override;
    void on_deactivate() override;
    virtual void on_exit() {}

    EventBasedActor(ActorContext* ctx, ActorSystem& sys);

  private:
    Behavior behavior_;
};

} // namespace hpactor