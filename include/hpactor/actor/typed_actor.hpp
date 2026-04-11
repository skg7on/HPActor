#pragma once

#include <memory>

#include <hpactor/actor/local_actor.hpp>
#include <hpactor/typed_behavior.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// typed_event_based_actor - statically typed event-based actor
// -----------------------------------------------------------------------------
template <typename... Signatures>
class typed_event_based_actor : public local_actor {
  public:
    using behavior_type = typed_behavior<Signatures...>;

    void become(behavior_type bh) {
        behavior_ = std::move(bh);
    }

    template <typename T>
    typename handler_type<T>::result operator()(T&& /*msg*/) {
        using handler_t = handler_type<T>;
        return typename handler_t::result{};
    }

  protected:
    virtual behavior_type make_behavior() = 0;

    typed_event_based_actor(ActorContext* ctx, ActorSystem& sys)
        : local_actor(ctx, sys) {}

    typed_event_based_actor(ActorId id, ActorContext* ctx, ActorSystem& sys)
        : local_actor(id, ctx, sys) {}

    void on_activate() override {}
    void on_deactivate() override {}

    void receive(MessageVariant&& /*msg*/) override {}

  private:
    behavior_type behavior_;
};

// -----------------------------------------------------------------------------
// typed_actor - type-safe reference to a typed_event_based_actor
// -----------------------------------------------------------------------------
template <typename... Signatures> class typed_actor {
  public:
    using base_type = typed_event_based_actor<Signatures...>;

    typed_actor() = default;
    explicit typed_actor(std::shared_ptr<base_type> ptr)
        : actor_(std::move(ptr)) {}

    template <typename T> void operator()(T&& msg) {
        if (actor_) {
            (*actor_)(std::forward<T>(msg));
        }
    }

    ActorId id() const {
        if (actor_) {
            return actor_->id();
        }
        return ActorId{};
    }

    ActorAddress address() const {
        if (actor_) {
            return actor_->address();
        }
        return ActorAddress{};
    }

    operator ActorAddress() const {
        return address();
    }

    explicit operator bool() const {
        return actor_ != nullptr;
    }

  private:
    std::shared_ptr<base_type> actor_;
};

} // namespace hpactor