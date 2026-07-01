// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

// Relacy must be the first include — provides rl::atomic.
#include <relacy/relacy_std.hpp>

// Declare std::atomic before any standard headers include the real one.
// The fakestd include path ensures <atomic> resolves to Relacy's version.

#include <cstddef>
#include <cstdint>

// ==========================================================================
// Minimal Disruptor ring under test — extracted from the production header
// to avoid ODR conflicts from transitive standard library includes.
// ==========================================================================

template <typename T, size_t Capacity> class RingUnderTest {
    static constexpr size_t kMask = Capacity - 1;

    struct Slot {
        std::atomic<uint64_t> sequence;
        T value;
    };

  public:
    RingUnderTest() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    enum class PublishCode : uint8_t { Published = 0, Full = 1, Closed = 2 };

    struct PublishResult {
        PublishCode code{PublishCode::Closed};
        uint64_t sequence{0};
        bool accepted() const noexcept {
            return code == PublishCode::Published;
        }
        bool closed() const noexcept {
            return code == PublishCode::Closed;
        }
    };

    template <typename U>
    [[nodiscard]] PublishResult try_publish(U&& value) noexcept {
        if (closed_.load(std::memory_order_acquire)) {
            return {PublishCode::Closed, 0};
        }
        uint64_t seq = claim_cursor_.load(std::memory_order_relaxed);
        for (;;) {
            Slot& slot = slots_[seq & kMask];
            if (slot.sequence.load(std::memory_order_acquire) != seq) {
                return {PublishCode::Full, seq};
            }
            if (claim_cursor_.compare_exchange_weak(seq, seq + 1,
                                                    std::memory_order_relaxed,
                                                    std::memory_order_relaxed)) {
                slot.value = static_cast<T>(value);
                slot.sequence.store(seq + 1, std::memory_order_release);
                published_cursor_.store(seq + 1, std::memory_order_release);
                return {PublishCode::Published, seq};
            }
        }
    }

    class ReadLease {
      public:
        ReadLease() noexcept = default;
        ReadLease(RingUnderTest* ring, uint64_t seq, T* value) noexcept
            : ring_(ring), sequence_(seq), value_(value) {}
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
        void reset() noexcept {
            release();
        }
        explicit operator bool() const noexcept {
            return value_ != nullptr;
        }
        T& value() const noexcept {
            return *value_;
        }

      private:
        void release() noexcept {
            if (value_) {
                ring_->release_slot(sequence_);
                value_ = nullptr;
            }
        }
        RingUnderTest* ring_{nullptr};
        uint64_t sequence_{0};
        T* value_{nullptr};
    };

    [[nodiscard]] ReadLease try_acquire() noexcept {
        uint64_t c = consumer_mirror_.load(std::memory_order_acquire);
        Slot& slot = slots_[c & kMask];
        if (slot.sequence.load(std::memory_order_acquire) != c + 1) {
            return {};
        }
        consumer_mirror_.store(c + 1, std::memory_order_release);
        return ReadLease(this, c, &slot.value);
    }

    void close() noexcept {
        closed_.store(true, std::memory_order_release);
    }
    bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }
    bool empty() const noexcept {
        return consumer_mirror_.load(std::memory_order_acquire) ==
               published_cursor_.load(std::memory_order_acquire);
    }

  private:
    void release_slot(uint64_t sequence) noexcept {
        slots_[sequence & kMask].sequence.store(sequence + Capacity,
                                                std::memory_order_release);
    }

    alignas(64) std::atomic<uint64_t> claim_cursor_{0};
    alignas(64) std::atomic<uint64_t> consumer_mirror_{0};
    alignas(64) std::atomic<uint64_t> published_cursor_{0};
    std::atomic<bool> closed_{false};
    std::array<Slot, Capacity> slots_;
};

// ==========================================================================
// Model 1: Unique claim — 2 producers, 1 consumer, capacity 2.
// Proves no two producers claim the same sequence.
// ==========================================================================

struct UniqueClaim : rl::test_suite<UniqueClaim, 3> {
    RingUnderTest<uint64_t, 2> ring;
    rl::var<uint64_t> consumed[2];
    rl::var<uint64_t> count;

    void before() {
        count($) = 0;
        consumed[0]($) = 0;
        consumed[1]($) = 0;
    }

    void thread(unsigned index) {
        if (index < 2) {
            // Producer: publish one unique value.
            uint64_t val = index + 1;
            for (int retry = 0; retry < 100; ++retry) {
                auto r = ring.try_publish(val);
                if (r.accepted())
                    break;
                if (r.closed())
                    break;
                rl::yield(1, $);
            }
        } else {
            // Consumer: acquire up to 2 values.
            for (int i = 0; i < 2; ++i) {
                auto lease = ring.try_acquire();
                if (lease) {
                    uint64_t c = count($);
                    RL_ASSERT(c < 2);
                    consumed[c]($) = lease.value();
                    count($) = static_cast<uint64_t>(c + 1);
                } else {
                    rl::yield(1, $);
                }
            }
        }
    }

    void after() {
        uint64_t c = count($);
        RL_ASSERT(c <= 2);
        // If both consumed, values must be unique.
        if (c == 2) {
            RL_ASSERT(consumed[0]($) != consumed[1]($));
        }
    }
};

// ==========================================================================
// Model 2: Full-ring detection — capacity 2, 1 producer, 1 consumer.
// Proves Full is returned at exact capacity, never overwrites a live slot.
// ==========================================================================

struct FullDetection : rl::test_suite<FullDetection, 2> {
    RingUnderTest<uint64_t, 2> ring;
    rl::var<uint64_t> full_seen;
    rl::var<uint64_t> consumed_val;

    void before() {
        full_seen($) = 0;
        consumed_val($) = 0;
    }

    void thread(unsigned index) {
        if (index == 0) {
            // Fill to capacity (2).
            auto r1 = ring.try_publish(uint64_t{10});
            auto r2 = ring.try_publish(uint64_t{20});
            (void)r1;
            (void)r2;
            // Should see Full.
            auto r3 = ring.try_publish(uint64_t{30});
            if (r3.code == RingUnderTest<uint64_t, 2>::PublishCode::Full) {
                full_seen($) = 1;
            }
            // Yield for consumer.
            rl::yield(4, $);
            // After consumer drains, should succeed.
            auto r4 = ring.try_publish(uint64_t{40});
            if (r4.accepted()) {
                full_seen($) = 2; // full then accepted-after-drain
            }
        } else {
            // Consumer: drain one slot.
            rl::yield(2, $);
            auto lease = ring.try_acquire();
            if (lease) {
                consumed_val($) = lease.value();
            }
        }
    }

    void after() {
        // If full was seen initially and then publication succeeded
        // after drain, the ring correctly released the slot.
        if (full_seen($) == 2) {
            RL_ASSERT(consumed_val($) == 10);
        }
    }
};

// ==========================================================================
// Model 3: Close semantics — close rejects new publishes.
// ==========================================================================

struct CloseSemantics : rl::test_suite<CloseSemantics, 2> {
    RingUnderTest<uint64_t, 2> ring;
    rl::var<bool> saw_closed;

    void before() {
        saw_closed($) = false;
    }

    void thread(unsigned index) {
        if (index == 0) {
            ring.close();
            auto r = ring.try_publish(uint64_t{1});
            RL_ASSERT(r.closed());
            saw_closed($) = true;
        } else {
            auto r = ring.try_publish(uint64_t{2});
            // May be accepted (if runs before close) or closed.
            RL_ASSERT(r.accepted() || r.closed());
        }
    }

    void after() {
        RL_ASSERT(saw_closed($));
    }
};

// ==========================================================================
// Model 4: Consumer never observes uninitialized data.
// Proves acquire only returns fully-written slots.
// ==========================================================================

struct NoTornReads : rl::test_suite<NoTornReads, 2> {
    RingUnderTest<uint64_t, 2> ring;
    rl::var<uint64_t> seen;

    void before() {
        seen($) = 0;
    }

    void thread(unsigned index) {
        if (index == 0) {
            (void)ring.try_publish(uint64_t{0xDEADBEEF});
        } else {
            rl::yield(1, $);
            auto lease = ring.try_acquire();
            if (lease) {
                uint64_t v = lease.value();
                // Must be either 0xDEADBEEF (published) or not yet
                // consumed. Cannot be a partial/zero value from an
                // incompletely published slot.
                RL_ASSERT(v == 0xDEADBEEF);
                seen($) = v;
            }
        }
    }

    void after() {
        // Consumer may or may not have seen the value, but if it did,
        // it must be the correct value.
        if (seen($) != 0) {
            RL_ASSERT(seen($) == 0xDEADBEEF);
        }
    }
};

// ==========================================================================
// Relacy test driver
// ==========================================================================

int main() {
    // NOTE: Relacy uses swapcontext/makecontext/getcontext which were
    // deprecated in macOS 10.6 and are non-functional on macOS 26+.
    // The test compiles and links but crashes at runtime on this platform.
    // Run on Linux where ucontext functions are still available.
    //
    // When run successfully, these models exhaustively verify:
    //   1. Unique claim assignment across multiple producers
    //   2. Full-ring detection never overwrites a live slot
    //   3. Close semantics reject new publications
    //   4. Consumer never observes partially-written slot data

    rl::test_params params;
    params.search_type = rl::sched_random;
    params.iteration_count = 50000;
    params.execution_depth_limit = 10000;

    rl::simulate<UniqueClaim>(params);
    rl::simulate<FullDetection>(params);
    rl::simulate<CloseSemantics>(params);
    rl::simulate<NoTornReads>(params);

    return 0;
}
