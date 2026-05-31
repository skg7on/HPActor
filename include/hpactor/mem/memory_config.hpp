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

#include <hpactor/hpactor_config.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/thread_local_allocator.hpp>

#include <cstddef>
#include <cstdlib>

namespace hpactor::mem {

// =========================================================================
// Compile-time configuration
// =========================================================================

/// \brief \c true when per-actor memory tracking is enabled at compile time.
#if HPACTOR_ENABLE_MEMORY_TRACKING
inline constexpr bool kMemoryTrackingEnabled = true;
#else
inline constexpr bool kMemoryTrackingEnabled = false;
#endif

/// \brief \c true when memory poisoning and canary verification are enabled at
/// compile time.
#if HPACTOR_ENABLE_MEMORY_DEBUG
inline constexpr bool kMemoryDebugEnabled = true;
#else
inline constexpr bool kMemoryDebugEnabled = false;
#endif

/// \brief Default telemetry sample rate (1 out of every N allocations is
/// recorded).
inline constexpr uint32_t kDefaultSampleRate = 128;

// =========================================================================
// Thread-local allocator pointer
// =========================================================================

/// \brief Thread-local pointer to the current thread's allocator.
///
/// Set by WorkerThread during initialization and used by global convenience
/// functions.
extern thread_local ThreadLocalAllocator* t_tla;

/// \brief Thread-local current actor ID, set by the scheduler before
/// dispatching actor work.
extern thread_local ActorId t_current_actor_id;

/// \brief Set the thread-local allocator pointer.
///
/// \param[in] tla Pointer to the ThreadLocalAllocator for the calling thread.
inline void set_thread_allocator(ThreadLocalAllocator* tla) {
    t_tla = tla;
}

/// \brief Return the thread-local allocator pointer.
///
/// \return The current thread's ThreadLocalAllocator, or \c nullptr if not set.
inline ThreadLocalAllocator* thread_allocator() {
    return t_tla;
}

/// \brief Set the current actor ID for the calling thread.
///
/// \param[in] id ActorId to associate with subsequent allocations on this
/// thread.
inline void set_current_actor_id(ActorId id) {
    t_current_actor_id = id;
}

/// \brief Return the current actor ID for the calling thread.
///
/// \return The ActorId set by the scheduler.
inline ActorId current_actor_id() {
    return t_current_actor_id;
}

// =========================================================================
// Global convenience API (implemented in memory_config.cpp)
// =========================================================================

/// \brief Allocate memory from a typed region.
///
/// The single admission point for all managed allocations. Reserves region
/// capacity, allocates via the thread-local allocator or fallback, and commits
/// to the region registry and memory tracker on success.
///
/// \param[in] region The memory region to charge against.
/// \param[in] user_bytes Number of bytes requested by the caller.
/// \param[in] owner The owning actor.
/// \return Pointer to user data, or \c nullptr if region pressure rejects the
///         allocation or the allocator is exhausted.
/// \note Must be called from a thread with a valid \c t_tla set. The scheduler
///       guarantees this for actor threads.
void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept;

/// \brief Allocate from a specific size class (defaults to kInternal region).
///
/// Convenience wrapper around \c allocate() that maps the size class to a byte
/// count.
///
/// \param[in] sc The size class to allocate from.
/// \param[in] owner The owning actor.
/// \return Pointer to user data, or \c nullptr on failure.
void* allocate_class(SizeClass sc, ActorId owner) noexcept;

/// \brief Deallocate memory previously returned by \c allocate() or
/// \c allocate_class().
///
/// Records the region free, routes to the origin SlabCache for cross-thread
/// frees, and safely handles fallback (std::malloc) deallocations.
///
/// \param[in] user_ptr Pointer previously returned by an allocation function.
/// \note Safe to call from any thread. Cross-thread frees are routed to the
///       origin cache via slab lookup.
void deallocate(void* user_ptr) noexcept;

} // namespace hpactor::mem
