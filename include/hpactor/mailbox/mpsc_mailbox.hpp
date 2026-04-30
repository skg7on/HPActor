// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <atomic>

namespace hpactor::mailbox {

// Intrusive Vyukov MPSC queue for actor mailboxes.
// T must provide: std::atomic<T*> mpsc_next
//
// Uses head/tail pointers with a dummy stub node. The dummy's mpsc_next
// always points to the oldest element (or nullptr when empty).
//
// Queue structure:
//   head_ (dummy) -> oldest -> ... -> newest -> nullptr
//   tail_ always points to the dummy stub
//   count_ tracks number of elements (producer increments, consumer decrements)
//
// The dummy node is a T (inherited) so &stub_ is a valid T*.
//
template <typename T> class MPSCMailbox {
  public:
    MPSCMailbox() {
        stub_.mpsc_next.store(nullptr, std::memory_order_relaxed);
        head_.store(&stub_, std::memory_order_relaxed);
        tail_.store(&stub_, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
    }

    // Producer: enqueue node at head (wait-free)
    // Chain: head_ -> node -> old_head -> ... -> oldest -> nullptr
    void enqueue(T* node) noexcept {
        node->mpsc_next.store(nullptr, std::memory_order_relaxed);
        T* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->mpsc_next.store(node, std::memory_order_release);
        count_.fetch_add(1, std::memory_order_release);
    }

    // Consumer: dequeue oldest node, or nullptr if empty.
    T* dequeue() noexcept {
        for (;;) {
            // Quick return path for empty queue.
            if (count_.load(std::memory_order_acquire) == 0)
                return nullptr;

            T* tail = tail_.load(std::memory_order_acquire);
            T* next = tail->mpsc_next.load(std::memory_order_acquire);
            if (next == nullptr) {
                // Count says non-empty but stub->next is null:
                // elements are orphaned.  Try to recover through
                // the last-dequeued node's forward link.
                T* ld = last_dequeued_.load(std::memory_order_acquire);
                if (ld) {
                    next = ld->mpsc_next.load(std::memory_order_acquire);
                    if (next) {
                        // Restore the chain: stub -> recovered
                        tail->mpsc_next.store(next,
                                              std::memory_order_release);
                        continue; // retry from the top
                    }
                }
                return nullptr;
            }

            T* next_next = next->mpsc_next.load(std::memory_order_relaxed);
            if (next_next == nullptr) {
                T* head = head_.load(std::memory_order_acquire);
                if (head != next) {
                    do {
                        next_next = next->mpsc_next.load(
                            std::memory_order_acquire);
                    } while (next_next == nullptr);
                }
            }

            tail->mpsc_next.store(next_next, std::memory_order_release);

            if (next_next == nullptr) {
                T* head = head_.load(std::memory_order_acquire);
                if (head != next) {
                    next_next = next->mpsc_next.load(
                        std::memory_order_acquire);
                    if (next_next) {
                        tail->mpsc_next.store(next_next,
                                              std::memory_order_release);
                    }
                }
            }

            if (next_next == tail) {
                tail->mpsc_next.store(nullptr, std::memory_order_release);
            }

            // Save last dequeued for orphan recovery.
            last_dequeued_.store(next, std::memory_order_release);
            count_.fetch_sub(1, std::memory_order_release);
            return next;
        }
    }

    // Consumer: try dequeue once (non-blocking) — same as dequeue
    T* try_dequeue() noexcept {
        return dequeue();
    }

    bool empty() const noexcept {
        return count_.load(std::memory_order_acquire) == 0;
    }

  private:
    struct Stub : public T {
        Stub() : T() {}
    };

    alignas(64) std::atomic<T*> head_; // newest element
    alignas(64) std::atomic<T*> tail_; // always stub (dummy)
    alignas(64) Stub stub_;            // dummy anchor
    std::atomic<int64_t> count_;       // element count
    std::atomic<T*> last_dequeued_{nullptr}; // for orphan recovery
};

} // namespace hpactor::mailbox
