#pragma once
#include <cstddef>
#include <hpactor/message.hpp>

namespace hpactor {

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

}