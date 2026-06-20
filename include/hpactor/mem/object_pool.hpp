// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// ...

#pragma once

#include <array>
#include <atomic>
#include <cstddef>

#include <hpactor/mem/memory_config.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::mem {

/// \brief Fixed-size, lock-free object pool with prefill and acquire/release.
///
/// Designed for single-consumer use (one actor thread). Objects are
/// pre-allocated via \c prefill() and recycled via \c release(). When the
/// pool is exhausted, \c try_acquire() returns \c nullptr — the caller
/// should fall back to \c mem::allocate() or \c operator new.
///
/// \tparam T  Object type stored in the pool.
/// \tparam N  Maximum pool capacity (default 256).
///
/// \note Thread safety: \c try_acquire() and \c release() use relaxed
///       atomics and are safe from a single producer thread. Concurrent
///       acquire/release from multiple threads requires external
///       synchronization.
template <typename T, size_t N = 256> class ObjectPool {
  public:
    ObjectPool() = default;
    ~ObjectPool() = default;

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    /// \brief Pre-allocate \p n objects via \c mem::allocate().
    ///
    /// Call once before the hot path. Objects are placement-new'd with
    /// their default constructor.
    ///
    /// \param[in] n Number of objects to pre-allocate (clamped to N).
    void prefill(size_t n) {
        if (n > N)
            n = N;
        for (size_t i = 0; i < n; ++i) {
            void* raw = mem::allocate(mem::RegionType::kMessage, sizeof(T),
                                      hpactor::ActorId{});
            auto* obj = new (raw) T();
            slots_[i] = obj;
        }
        count_.store(n, std::memory_order_release);
    }

    /// \brief Try to acquire an object from the pool.
    ///
    /// \return Pointer to an object, or \c nullptr if the pool is empty.
    ///         The caller owns the returned object until \c release() is
    ///         called or the object is deallocated externally.
    T* try_acquire() noexcept {
        size_t n = count_.load(std::memory_order_acquire);
        if (n == 0)
            return nullptr;
        // Decrement count and return the last available slot.
        size_t new_n = n - 1;
        count_.store(new_n, std::memory_order_release);
        return slots_[new_n];
    }

    /// \brief Return an object to the pool for reuse.
    ///
    /// If the pool is at capacity, the object is silently deallocated.
    ///
    /// \param[in] obj Pointer to the object to return. Must not be \c nullptr.
    /// \note The object's destructor is NOT called; the caller should reset
    ///       the object to a known state before releasing.
    void release(T* obj) noexcept {
        size_t n = count_.load(std::memory_order_acquire);
        if (n >= N) {
            // Pool full — deallocate.
            mem::deallocate(obj);
            return;
        }
        slots_[n] = obj;
        count_.store(n + 1, std::memory_order_release);
    }

    /// \brief Number of objects currently available in the pool.
    ///
    /// \return Count of pre-allocated objects ready for \c try_acquire().
    size_t available() const noexcept {
        return count_.load(std::memory_order_acquire);
    }

  private:
    std::array<T*, N> slots_{};
    std::atomic<size_t> count_{0};
};

} // namespace hpactor::mem
