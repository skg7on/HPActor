#pragma once

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/behavior.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// event_based_actor - cooperatively scheduled actor with behavior-based handling
// -----------------------------------------------------------------------------
class event_based_actor : public local_actor {
public:
    void become(Behavior bh);
    void become_empty();

    void receive(MessageVariant&& msg) override;

protected:
    virtual Behavior make_behavior() { return {}; }
    virtual void on_activate();
    virtual void on_deactivate();
    virtual void on_exit() {}

    event_based_actor(ActorContext* ctx, ActorSystem& sys);

private:
    Behavior behavior_;
};

} // namespace hpactor