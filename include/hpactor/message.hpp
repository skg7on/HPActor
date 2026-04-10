#pragma once
#include <utility>
#include <type_traits>

namespace hpactor {

// Error codes for hot-path operations (no exceptions)
enum class MailboxError {
    Success,
    Full,       // Mailbox at capacity
    Empty,      // Mailbox empty on pop
    Invalid     // Invalid state
};

template<typename T>
class Message {
public:
    Message() = default;

    // Perfect forwarding constructor - handles both copy and move
    template<typename U>
    explicit Message(U&& payload) : payload_(std::forward<U>(payload)) {}

    T& payload() noexcept { return payload_; }
    const T& payload() const noexcept { return payload_; }

    // Allow explicit access to underlying payload move
    T&& move_payload() noexcept { return std::move(payload_); }

private:
    T payload_;
};

}