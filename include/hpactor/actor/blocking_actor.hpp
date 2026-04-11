#pragma once

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/mailbox.hpp>
#include <hpactor/message.hpp>
#include <hpactor/types.hpp>

#include <chrono>
#include <condition_variable>
#include <mutex>

namespace hpactor {

// -----------------------------------------------------------------------------
// blocking_actor - actor that runs in its own thread with blocking receive
// -----------------------------------------------------------------------------
class blocking_actor : public local_actor {
  public:
    template <typename... Handlers> void receive(Handlers&&... handlers);

    template <typename T> void receive_for(T& begin, T end);

    template <typename... Actors>
    void wait_for(ActorAddr first, Actors&&... rest);

    void await_all_other_actors_done();

    const error& fail_state() const {
        return fail_state_;
    }
    void fail_state(error e) {
        fail_state_ = e;
    }

  protected:
    blocking_actor(ActorContext* ctx, ActorSystem& sys);
    blocking_actor(ActorId id, ActorContext* ctx, ActorSystem& sys);

    virtual void on_activate() override;
    virtual void on_deactivate() override;

  private:
    error fail_state_;
};

} // namespace hpactor