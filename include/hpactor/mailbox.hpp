#pragma once
#include <cstddef>
#include <memory>
#include <hpactor/message.hpp>

namespace hpactor {

// Forward declaration - full definition in mutex_mailbox.hpp
template<typename T>
class MutexMailbox;

template<typename T>
class IMailbox {
public:
    virtual ~IMailbox() = default;

    // Hot path - marked noexcept for real-time guarantees
    virtual void push(Message<T>&& msg) noexcept = 0;
    virtual bool try_pop(Message<T>& out) noexcept = 0;

    // Blocking pop - may block, used for actor message processing loop
    virtual bool pop(Message<T>& out) = 0;

    virtual size_t size() const = 0;
    virtual bool empty() const = 0;
};

// Default implementation selector
enum class MailboxType {
    Mutex,       // Thread-safe, slower
    LockFree,   // TODO: Implementation TBD after stress tests pass
};

// Factory function
template<typename T, MailboxType Type = MailboxType::Mutex>
std::unique_ptr<IMailbox<T>> create_mailbox() {
    if constexpr (Type == MailboxType::Mutex) {
        return std::make_unique<MutexMailbox<T>>();
    }
    // LockFree would be added here later
}

}