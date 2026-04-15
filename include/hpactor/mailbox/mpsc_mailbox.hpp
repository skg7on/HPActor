// include/hpactor/mailbox/mpsc_mailbox.hpp
#pragma once

#include <atomic>
#include <cstddef>

namespace hpactor::mailbox {

// Intrusive Vyukov MPSC queue for actor mailboxes
// T must provide: std::atomic<T*> mpsc_next
template<typename T>
class MPSCMailbox {
public:
    MPSCMailbox() {
        stub_.mpsc_next.store(nullptr, std::memory_order_relaxed);
        head_.store(&stub_, std::memory_order_relaxed);
        tail_.store(&stub_, std::memory_order_relaxed);
    }

    // Producer: enqueue node (wait-free)
    void enqueue(T* node) noexcept {
        node->mpsc_next.store(nullptr, std::memory_order_relaxed);
        T* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->mpsc_next.store(node, std::memory_order_release);
    }

    // Consumer: dequeue node (lock-free, single consumer)
    T* dequeue() noexcept {
        T* tail = tail_.load(std::memory_order_acquire);
        T* next = tail->mpsc_next.load(std::memory_order_acquire);

        if (tail == &stub_) {
            if (!next) return nullptr;
            tail_ = next;
            tail = next;
            next = tail->mpsc_next.load(std::memory_order_acquire);
        }

        if (next) {
            tail_.store(next, std::memory_order_release);
            return tail;
        }

        return nullptr;
    }

    // Check if empty
    bool empty() const noexcept {
        T* tail = tail_.load(std::memory_order_acquire);
        return tail == &stub_ && tail->mpsc_next.load(std::memory_order_acquire) == nullptr;
    }

private:
    struct Stub {
        std::atomic<T*> mpsc_next{nullptr};
    };

    alignas(64) std::atomic<T*> head_;
    alignas(64) std::atomic<T*> tail_;
    alignas(64) Stub stub_;
};

} // namespace hpactor::mailbox
