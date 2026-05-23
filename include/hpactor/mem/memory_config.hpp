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

#if HPACTOR_ENABLE_MEMORY_TRACKING
inline constexpr bool kMemoryTrackingEnabled = true;
#else
inline constexpr bool kMemoryTrackingEnabled = false;
#endif

#if HPACTOR_ENABLE_MEMORY_DEBUG
inline constexpr bool kMemoryDebugEnabled = true;
#else
inline constexpr bool kMemoryDebugEnabled = false;
#endif

inline constexpr uint32_t kDefaultSampleRate = 128;

// =========================================================================
// Thread-local allocator pointer
// =========================================================================

// Set by WorkerThread during init, used by global convenience functions.
extern thread_local ThreadLocalAllocator* t_tla;

// Current actor ID — set by the scheduler before dispatching actor work.
extern thread_local ActorId t_current_actor_id;

inline void set_thread_allocator(ThreadLocalAllocator* tla) {
    t_tla = tla;
}

inline ThreadLocalAllocator* thread_allocator() {
    return t_tla;
}

inline void set_current_actor_id(ActorId id) {
    t_current_actor_id = id;
}

inline ActorId current_actor_id() {
    return t_current_actor_id;
}

// =========================================================================
// Global convenience API (implemented in memory_config.cpp)
// =========================================================================

// Allocate memory from a typed region. The single admission point for all
// managed allocations: reserves region capacity, allocates via TLA or
// fallback, commits to region registry and memory tracker.
void* allocate(RegionType region, size_t user_bytes, ActorId owner) noexcept;

// Allocate from a specific size class (defaults to kInternal region).
void* allocate_class(SizeClass sc, ActorId owner) noexcept;

// Deallocate memory. Records region free, routes to origin cache for
// cross-thread frees, handles fallback deallocations safely.
void deallocate(void* user_ptr) noexcept;

} // namespace hpactor::mem
