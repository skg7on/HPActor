#pragma once

#include <functional>
#include <hpactor/message.hpp>

namespace hpactor {

// -----------------------------------------------------------------------------
// Behavior - handler function for message processing
// -----------------------------------------------------------------------------
class Behavior {
  public:
    using handler_type = std::function<void(MessageVariant&&)>;

    Behavior() = default;

    explicit Behavior(handler_type handler) : handler_(std::move(handler)) {}

    explicit operator bool() const {
        return handler_ != nullptr;
    }

    void operator()(MessageVariant&& msg) const {
        if (handler_) {
            handler_(std::move(msg));
        }
    }

  private:
    handler_type handler_;
};

} // namespace hpactor