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
#include <thread>

namespace hpactor::adt {

// Intrusive Vyukov MPSC queue.
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
        // Full barrier: on ARM64 the exchange(acq_rel) may become
        // visible before the mpsc_next store below, causing the
        // consumer to see head_ updated but tail_->mpsc_next == nullptr
        // and spin forever.  The seq_cst fence forces the exchange to
        // drain before the store, so the consumer always sees the
        // updated mpsc_next when it observes the new head_.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        prev->mpsc_next.store(node, std::memory_order_release);
        count_.fetch_add(1, std::memory_order_release);
    }

    // Consumer: dequeue oldest node, or nullptr if empty.
    //
    // Single-consumer: only one thread may call dequeue() at a time.
    // Callers must serialize through an external lock when the consumer
    // path can be entered from multiple contexts (e.g. normal dispatch
    // and overflow-driven drop_one_oldest).
    T* dequeue() noexcept {
        T* t = tail_.load(std::memory_order_acquire);
        T* next = t->mpsc_next.load(std::memory_order_acquire);

        if (next == nullptr) {
            if (head_.load(std::memory_order_acquire) == t)
                return nullptr;
            // Bounded spin: the producer has updated head_ but not yet
            // written mpsc_next.  Yield after a threshold to let the
            // producer thread run rather than spinning forever.
            for (int spin = 0;; ++spin) {
                next = t->mpsc_next.load(std::memory_order_acquire);
                if (next != nullptr)
                    break;
                if (spin >= 1000) {
                    std::this_thread::yield();
                    spin = 0;
                }
            }
        }

        T* next_next = next->mpsc_next.load(std::memory_order_relaxed);
        if (next_next == nullptr) {
            T* h = head_.load(std::memory_order_acquire);
            if (h != next) {
                for (int spin = 0;; ++spin) {
                    next_next = next->mpsc_next.load(std::memory_order_acquire);
                    if (next_next != nullptr)
                        break;
                    if (spin >= 1000) {
                        std::this_thread::yield();
                        spin = 0;
                    }
                }
            }
        }

        t->mpsc_next.store(next_next, std::memory_order_release);

        if (next_next == nullptr) {
            T* h = head_.load(std::memory_order_acquire);
            if (h != next) {
                // Producer enqueued between the first head_ check (line 72)
                // and the chain update (line 80).  The node we are about to
                // return is no longer the head — a new node was inserted
                // behind it.  Spin until the producer completes its
                // mpsc_next store so we can correctly link stub_ to the new
                // head.  Without this spin, t->mpsc_next stays nullptr while
                // head_ points past the stub, causing the next dequeue() to
                // spin forever in the first wait loop (lines 65–67).
                for (int spin = 0;; ++spin) {
                    next_next = next->mpsc_next.load(std::memory_order_acquire);
                    if (next_next != nullptr)
                        break;
                    if (spin >= 1000) {
                        std::this_thread::yield();
                        spin = 0;
                    }
                }
                t->mpsc_next.store(next_next, std::memory_order_release);
            }
        }

        if (next_next == t) {
            t->mpsc_next.store(nullptr, std::memory_order_release);
        }

        {
            T* h = head_.load(std::memory_order_acquire);
            if (h == next) {
                if (!head_.compare_exchange_strong(next, static_cast<T*>(&stub_),
                                                   std::memory_order_release,
                                                   std::memory_order_relaxed)) {
                    T* updated = next->mpsc_next.load(std::memory_order_acquire);
                    if (updated && updated != next_next) {
                        if (next_next) {
                            T* tail = next_next;
                            T* tn;
                            while ((tn = tail->mpsc_next.load(
                                        std::memory_order_acquire)) != nullptr) {
                                tail = tn;
                            }
                            tail->mpsc_next.store(updated,
                                                  std::memory_order_release);
                        } else {
                            t->mpsc_next.store(updated, std::memory_order_release);
                        }
                    }
                }
            }
        }

        count_.fetch_sub(1, std::memory_order_release);
        return next;
    }

    // Consumer: try dequeue once (non-blocking) — same as dequeue
    T* try_dequeue() noexcept {
        return dequeue();
    }

    bool empty() const noexcept {
        return count_.load(std::memory_order_acquire) == 0;
    }

    int64_t count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

  private:
    struct Stub : public T {
        Stub() : T() {}
    };

    alignas(64) std::atomic<T*> head_; // newest element
    alignas(64) std::atomic<T*> tail_; // always stub (dummy)
    alignas(64) Stub stub_;            // dummy anchor
    std::atomic<int64_t> count_;       // element count
};

} // namespace hpactor::adt
