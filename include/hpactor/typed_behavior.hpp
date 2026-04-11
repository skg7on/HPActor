#pragma once

#include <functional>
#include <tuple>
#include <type_traits>
#include <variant>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/types.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// Forward declarations
// -----------------------------------------------------------------------------
template <typename Signature> class message_handler;

// -----------------------------------------------------------------------------
// handler_type - extracts result type from a typed message signature
// -----------------------------------------------------------------------------
template <typename T> struct handler_type;

template <typename R, typename Msg> struct handler_type<result<R>(Msg)> {
    using result = R;
    using message = Msg;

    template <typename F> result operator()(F&& f, Msg&& msg) {
        return f(std::move(msg));
    }
};

// -----------------------------------------------------------------------------
// typed_behavior - statically typed behavior for typed actors
// -----------------------------------------------------------------------------
template <typename... Signatures> class typed_behavior {
  public:
    using result_type = void;

    typed_behavior() = default;

    template <typename T> typed_behavior& operator()(T&& /*handler*/) {
        return *this;
    }

    result<void> invoke(MessageVariant& /*msg*/) {
        return result<void>::make();
    }

    bool matches(const MessageVariant& /*msg*/) const {
        return false;
    }

  private:
    std::tuple<message_handler<Signatures>...> handlers_;
};

// -----------------------------------------------------------------------------
// message_handler - handler for a single typed signature
// -----------------------------------------------------------------------------
template <typename R, typename Msg> class message_handler<result<R>(Msg)> {
  public:
    using signature = result<R>(Msg);

    message_handler() = default;

    template <typename F> explicit message_handler(F&& /*func*/) {}

    result<R> operator()(Msg&& /*msg*/) {
        return result<R>::make(error{});
    }

    bool matches(const MessageVariant& /*msg*/) const {
        return false;
    }
};

} // namespace hpactor