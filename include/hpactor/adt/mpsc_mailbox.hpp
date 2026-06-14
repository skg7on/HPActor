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

namespace hpactor::adt {

/// \brief Intrusive Vyukov-style MPSC lock-free queue.
///
/// A multi-producer, single-consumer queue using a dummy stub node to anchor
/// the chain.  Producers insert at the head (LIFO insertion order); the
/// consumer drains from the tail (FIFO removal order), so overall the queue
/// behaves FIFO.
///
/// Queue structure:
/// \code{.unparsed}
///   head_ (dummy stub) -> oldest -> ... -> newest -> nullptr
///   tail_ always points to the dummy stub
/// \endcode
///
/// The dummy stub is a private `Stub` that inherits from `T`, so `&stub_` is
/// a valid `T*` and the consumer never dequeues the stub itself.
///
/// \tparam T Element type.  Must provide a public member
///           `std::atomic<T*> mpsc_next` used as the intrusive link field.
///           The queue itself does not allocate or own nodes — callers are
///           responsible for allocation before enqueue and destruction after
///           dequeue.
///
/// \note **Thread safety:** Safe for many concurrent producers calling
///       `enqueue()`.  Exactly one consumer context may call `dequeue()` or
///       `try_dequeue()` at a time; if more than one code path can consume or
///       evict (e.g. normal dispatch and overflow-driven eviction), the owner
///       must serialize those paths externally.
///
/// \note **ARM64 visibility:** On weakly-ordered architectures the producer's
///       `head_.exchange(acq_rel)` may become visible before the subsequent
///       `mpsc_next.store(release)`.  When the consumer observes this
///       partially-completed enqueue it returns `nullptr` immediately rather
///       than spinning — the caller retries via the scheduler's
///       `RequeueReady` mechanism once the producer completes.  This is
///       deterministic; see PR #275 for the design history.
template <typename T> class MPSCMailbox {
  public:
    /// \brief Construct an empty queue.
    ///
    /// Initializes the dummy stub node and sets both \c head_ and \c tail_
    /// to point to it.  \c count_ starts at zero.
    MPSCMailbox() {
        stub_.mpsc_next.store(nullptr, std::memory_order_relaxed);
        head_.store(&stub_, std::memory_order_relaxed);
        tail_.store(&stub_, std::memory_order_relaxed);
        count_.store(0, std::memory_order_relaxed);
    }

    /// \brief Enqueue a node at the head (wait-free, multi-producer safe).
    ///
    /// Inserts \p node between the current head and the previous head,
    /// forming the chain: `head_ -> node -> old_head -> ...`.
    /// \c count_ is incremented after the link is published so that
    /// `empty()` is consistent with visibility of the enqueued element.
    ///
    /// \param[in] node Pointer to the element to enqueue.  Must not be
    ///                 `nullptr` and must not already be reachable from any
    ///                 queue lane.
    ///
    /// \note **Thread safety:** Lock-free and wait-free for concurrent
    ///       producers.  Uses `acq_rel` exchange + `release` store to
    ///       publish the link; the consumer's `acquire` loads form the
    ///       matching synchronizes-with edge.
    void enqueue(T* node) noexcept {
        node->mpsc_next.store(nullptr, std::memory_order_relaxed);
        T* prev = head_.exchange(node, std::memory_order_acq_rel);
        prev->mpsc_next.store(node, std::memory_order_release);
        count_.fetch_add(1, std::memory_order_release);
    }

    /// \brief Dequeue the oldest element from the queue.
    ///
    /// Traverses the chain from the dummy stub to locate the oldest
    /// reachable element, unlinks it, and returns it to the caller.
    /// Returns `nullptr` when the queue appears empty or when the consumer
    /// observes a partially-completed enqueue that has not yet published
    /// its `mpsc_next` link.
    ///
    /// \return Pointer to the dequeued element, or `nullptr`.
    /// \retval nullptr The queue is empty, OR the consumer observed a
    ///                 partially-completed enqueue (ARM64 visibility gap).
    ///                 The caller must treat `nullptr` as "retry later"
    ///                 rather than a definitive empty signal — the
    ///                 scheduler's `RequeueReady` mechanism re-dispatches
    ///                 the actor once the producer finishes.
    /// \retval non-nullptr A valid dequeued element.  The caller owns the
    ///                      node and is responsible for destruction.
    ///
    /// \note **Thread safety:** Single-consumer — only one thread may call
    ///       `dequeue()` or `try_dequeue()` at a time.  If more than one
    ///       code path can consume or evict (e.g. normal dispatch and
    ///       overflow-driven `drop_one_oldest`), the owner must serialize
    ///       those paths through an external lock.
    ///
    /// \note **ARM64:** The producer's `head_.exchange(acq_rel)` can become
    ///       visible before `mpsc_next.store(release)` on weakly-ordered
    ///       architectures.  Rather than spinning — which cannot make
    ///       progress when the producer thread is asleep — this method
    ///       returns `nullptr` immediately for any partially-completed
    ///       enqueue.  The caller retries; `count_` is incremented only
    ///       after the `mpsc_next` store, so `empty()` correctly reports
    ///       the queue as non-empty and the actor is re-dispatched.
    T* dequeue() noexcept {
        T* t = tail_.load(std::memory_order_acquire);
        T* next = t->mpsc_next.load(std::memory_order_acquire);

        if (next == nullptr) {
            if (head_.load(std::memory_order_acquire) == t)
                return nullptr;
            // Producer has exchanged head_ but mpsc_next isn't visible
            // yet — return nullptr, caller will retry.
            return nullptr;
        }

        T* next_next = next->mpsc_next.load(std::memory_order_relaxed);
        if (next_next == nullptr) {
            T* h = head_.load(std::memory_order_acquire);
            if (h != next)
                return nullptr; // producer hasn't linked the next node yet
        }

        t->mpsc_next.store(next_next, std::memory_order_release);

        if (next_next == nullptr) {
            T* h = head_.load(std::memory_order_acquire);
            if (h != next) {
                // Producer enqueued between head_ check and chain update.
                // Rather than spinning for mpsc_next, leave the chain as-is
                // and return nullptr — the next dequeue() will fix it up.
                return nullptr;
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

    /// \brief Try to dequeue one element (non-blocking).
    ///
    /// Identical to `dequeue()` — both are non-blocking.  Provided as a
    /// named alias for callers that want to emphasize the try-once
    /// semantics.
    ///
    /// \return Pointer to the dequeued element, or `nullptr`.
    /// \note **Thread safety:** Same single-consumer contract as
    ///       `dequeue()`.
    T* try_dequeue() noexcept {
        return dequeue();
    }

    /// \brief Check whether the queue appears empty.
    ///
    /// Reads \c count_ with `acquire` ordering, so it reflects all
    /// completed enqueues and dequeues that happened before the call.
    ///
    /// \return `true` if \c count_ is zero.
    /// \note This is a snapshot — a concurrent producer may enqueue
    ///       immediately after the load.  Do not use as a stable iteration
    ///       guard; prefer the `nullptr` return from `dequeue()` for
    ///       drain-loop termination.
    bool empty() const noexcept {
        return count_.load(std::memory_order_acquire) == 0;
    }

    /// \brief Return the approximate number of elements in the queue.
    ///
    /// Reads \c count_ with `acquire` ordering.
    ///
    /// \return The current \c count_ value.
    /// \note This is a snapshot — concurrent producers and the consumer
    ///       may change the count before the caller inspects the result.
    ///       Useful for admission, pressure monitoring, observability, and
    ///       lost-wakeup checks, but not a precise size under contention.
    int64_t count() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

  private:
    /// \private Dummy node that inherits from \c T to provide a valid
    /// queue anchor without a separate allocation.
    struct Stub : public T {
        Stub() : T() {}
    };

    alignas(64) std::atomic<T*> head_; ///< Newest element (producer insertion
                                       ///< point).
    alignas(64) std::atomic<T*> tail_; ///< Always points to the dummy stub.
    alignas(64) Stub stub_;            ///< Dummy anchor node.
    std::atomic<int64_t> count_;       ///< Approximate element count.
};

} // namespace hpactor::adt
