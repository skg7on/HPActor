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

#include <hpactor/mailbox/mailbox_kind.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <type_traits>
#include <utility>

namespace hpactor::adt {

// ── Publish result ────────────────────────────────────────────────────────

/// \brief Outcome of a single \c try_publish() call.
enum class RingPublishCode : uint8_t {
    /// The value was published and is now visible to the consumer.
    Published = 0,

    /// The ring is full; no slot was available.
    Full = 1,

    /// The ring has been closed; no further publication is allowed.
    Closed = 2,
};

/// \brief Result of \c try_publish() carrying the code and logical sequence.
struct RingPublishResult {
    RingPublishCode code{RingPublishCode::Closed};
    uint64_t sequence{0};

    [[nodiscard]] bool accepted() const noexcept {
        return code == RingPublishCode::Published;
    }
    [[nodiscard]] bool closed() const noexcept {
        return code == RingPublishCode::Closed;
    }
};

// ── Ring snapshot ─────────────────────────────────────────────────────────

/// \brief Approximate snapshot of ring state for observability.
struct DisruptorRingSnapshot {
    uint32_t capacity{0};
    uint32_t slot_bytes{0};
    uint64_t published_depth{0};
    uint64_t max_observed_depth{0};
    uint64_t claim_cursor{0};
    uint64_t consumer_sequence{0};
    uint64_t claim_retries{0};
    uint64_t gap_observations{0};
    bool closed{false};
};

// ── Disruptor MPSC ring ───────────────────────────────────────────────────

/// \brief Lock-free multi-producer / single-consumer ring with per-slot
///        sequence numbers (LMAX Disruptor style).
///
/// Producers claim unique slots via a CAS on \c claim_cursor_, copy the
/// value into the claimed slot, then publish by storing the sequence number
/// with release semantics.  The single consumer acquires published slots
/// through a move-only \c ReadLease that holds the slot until released.
///
/// \tparam T      The type stored in each slot. Must be nothrow
///                movable or copyable.
/// \tparam Capacity Power-of-two ring capacity in [2,
/// kMaxDisruptorRingCapacity].
///
/// \note Thread safety:
///       - Multiple producers may call \c try_publish() concurrently.
///       - Only one consumer may call \c try_acquire() at a time.
///       - \c close() is safe to call from any thread.
///       - \c snapshot() and \c empty() are safe to call from any thread
///         but return approximate values under concurrent producers.
///
/// \note Memory ordering:
///       - Claim: CAS relaxed (the slot sequence guards exclusivity).
///       - Publish: release store so consumer observes the value.
///       - Acquire: acquire load to see the published value.
///       - Release: release store so next producer sees the freed slot.
///
/// \warning A producer preempted after claim and before publication creates
///          a FIFO gap. The consumer does not skip the gap. This is a
///          deliberate multi-producer Disruptor tradeoff.
template <typename T, size_t Capacity>
    requires(mailbox::valid_disruptor_ring_capacity(Capacity))
class DisruptorMpscRing final {
    static constexpr size_t kMask = Capacity - 1;

    struct Slot {
        std::atomic<uint64_t> sequence;
        T value;
    };

  public:
    /// \brief Construct the ring and initialize all slot sequences.
    ///
    /// Slot \c i starts with \c sequence == i, meaning it is available
    /// for publication with logical sequence \c i.
    DisruptorMpscRing() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    DisruptorMpscRing(const DisruptorMpscRing&) = delete;
    DisruptorMpscRing& operator=(const DisruptorMpscRing&) = delete;
    DisruptorMpscRing(DisruptorMpscRing&&) = delete;
    DisruptorMpscRing& operator=(DisruptorMpscRing&&) = delete;
    ~DisruptorMpscRing() = default;

    // ── Producer API ──────────────────────────────────────────────────

    /// \brief Try to publish a value into the ring.
    ///
    /// Claims a unique logical sequence, copies \p value into the
    /// exclusively held slot, and publishes it with release ordering.
    ///
    /// \param[in] value The value to publish. Must be nothrow assignable.
    /// \return \c Published with the assigned logical sequence, \c Full
    ///         if no slot is available, or \c Closed if the ring is closed.
    /// \note Lock-free but not wait-free (CAS retry on contention).
    template <typename U>
        requires std::is_nothrow_assignable_v<T&, U&&>
    [[nodiscard]] RingPublishResult try_publish(U&& value) noexcept {
        if (closed_.load(std::memory_order_acquire)) {
            return {RingPublishCode::Closed, 0};
        }

        uint64_t sequence = claim_cursor_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[sequence & kMask];
            if (slot.sequence.load(std::memory_order_acquire) != sequence) {
                return {RingPublishCode::Full, sequence};
            }
            if (claim_cursor_.compare_exchange_weak(sequence, sequence + 1,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                // Exclusively claimed slot `sequence`.
                slot.value = static_cast<T>(std::forward<U>(value));
                slot.sequence.store(sequence + 1, std::memory_order_release);
                published_cursor_.store(sequence + 1, std::memory_order_release);
                update_max_depth(sequence + 1);
                return {RingPublishCode::Published, sequence};
            }
            // CAS failed — another producer claimed a slot in between.
            // `sequence` was reloaded with the new cursor value by CAS.
            claim_retries_.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // ── Consumer API (single-consumer only) ────────────────────────────

    /// \brief Move-only read lease that holds a published ring slot.
    ///
    /// The lease keeps the slot reserved — producers cannot overwrite it
    /// until the lease is destroyed or \c reset().  Only the consumer
    /// thread may own a lease.
    class ReadLease {
      public:
        ReadLease() noexcept = default;

        ReadLease(DisruptorMpscRing* ring, uint64_t sequence, T* value) noexcept
            : ring_(ring), sequence_(sequence), value_(value) {}

        ReadLease(const ReadLease&) = delete;
        ReadLease& operator=(const ReadLease&) = delete;

        ReadLease(ReadLease&& other) noexcept
            : ring_(other.ring_), sequence_(other.sequence_),
              value_(other.value_) {
            other.ring_ = nullptr;
            other.value_ = nullptr;
        }

        ReadLease& operator=(ReadLease&& other) noexcept {
            if (this != &other) {
                release();
                ring_ = other.ring_;
                sequence_ = other.sequence_;
                value_ = other.value_;
                other.ring_ = nullptr;
                other.value_ = nullptr;
            }
            return *this;
        }

        ~ReadLease() {
            release();
        }

        /// \brief Explicitly release the slot without waiting for
        ///        destruction.
        void reset() noexcept {
            release();
        }

        /// \brief True if the lease owns a slot.
        [[nodiscard]] explicit operator bool() const noexcept {
            return value_ != nullptr;
        }

        /// \brief Reference to the leased value.
        /// \pre The lease must be valid.
        [[nodiscard]] T& value() const noexcept {
            return *value_;
        }

      private:
        void release() noexcept {
            if (value_) {
                ring_->release_slot(sequence_);
                value_ = nullptr;
            }
        }

        DisruptorMpscRing* ring_{nullptr};
        uint64_t sequence_{0};
        T* value_{nullptr};
    };

    /// \brief Try to acquire the next published slot.
    ///
    /// Performs one acquire load and returns a \c ReadLease if the
    /// next slot has been published.  Returns an invalid lease if no
    /// slot is ready.  Never spins.
    ///
    /// \pre Only the consumer may call this method.
    [[nodiscard]] ReadLease try_acquire() noexcept {
        uint64_t c = consumer_mirror_.load(std::memory_order_acquire);
        Slot& slot = slots_[c & kMask];
        if (slot.sequence.load(std::memory_order_acquire) != c + 1) {
            return {};
        }
        // Advance the consumer mirror so this slot cannot be re-acquired
        // while the lease is held.  The physical slot remains reserved
        // until release_slot() publishes c + Capacity.
        consumer_mirror_.store(c + 1, std::memory_order_release);
        return ReadLease(this, c, &slot.value);
    }

    // ── Lifecycle ──────────────────────────────────────────────────────

    /// \brief Close the ring to new publications.
    ///
    /// Idempotent.  Thread-safe.  Already-published slots remain
    /// consumable.
    void close() noexcept {
        closed_.store(true, std::memory_order_release);
    }

    /// \brief True when \c close() has been called.
    [[nodiscard]] bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    // ── Query ──────────────────────────────────────────────────────────

    /// \brief True when no published slots are available for the consumer.
    ///
    /// Approximate under concurrent producers.
    [[nodiscard]] bool empty() const noexcept {
        return consumer_mirror_.load(std::memory_order_acquire) ==
               published_cursor_.load(std::memory_order_acquire);
    }

    /// \brief Approximate snapshot of ring state.
    [[nodiscard]] DisruptorRingSnapshot snapshot() const noexcept {
        DisruptorRingSnapshot snap;
        snap.capacity = static_cast<uint32_t>(Capacity);
        snap.slot_bytes = static_cast<uint32_t>(sizeof(Slot));
        snap.published_depth = published_cursor_.load(std::memory_order_acquire) -
                               consumer_mirror_.load(std::memory_order_acquire);
        snap.max_observed_depth = max_depth_.load(std::memory_order_relaxed);
        snap.claim_cursor = claim_cursor_.load(std::memory_order_relaxed);
        snap.consumer_sequence = consumer_mirror_.load(std::memory_order_acquire);
        snap.claim_retries = claim_retries_.load(std::memory_order_relaxed);
        snap.gap_observations = gap_observations_.load(std::memory_order_relaxed);
        snap.closed = is_closed();
        return snap;
    }

  private:
    void release_slot(uint64_t sequence) noexcept {
        slots_[sequence & kMask].sequence.store(sequence + Capacity,
                                                std::memory_order_release);
    }

    void update_max_depth(uint64_t published_cursor) noexcept {
        uint64_t depth =
            published_cursor - consumer_mirror_.load(std::memory_order_acquire);
        uint64_t current = max_depth_.load(std::memory_order_relaxed);
        while (depth > current) {
            if (max_depth_.compare_exchange_weak(current, depth,
                                                 std::memory_order_relaxed,
                                                 std::memory_order_relaxed)) {
                break;
            }
        }
    }

    // Cache-line isolation: claim cursor and diagnostics on separate lines.
    alignas(64) std::atomic<uint64_t> claim_cursor_{0};
    alignas(64) std::atomic<uint64_t> consumer_mirror_{0};
    alignas(64) std::atomic<uint64_t> published_cursor_{0};
    std::atomic<uint64_t> max_depth_{0};
    std::atomic<uint64_t> claim_retries_{0};
    std::atomic<uint64_t> gap_observations_{0};
    std::atomic<bool> closed_{false};
    std::array<Slot, Capacity> slots_;
};

} // namespace hpactor::adt
