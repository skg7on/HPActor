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

#include <hpactor/mem/thread_local_allocator.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/hpactor_config.hpp>

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
// Used by SlabAllocated::operator new to attribute allocations correctly.
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
// Global convenience API
// =========================================================================

// Allocate memory from a typed region, auto-selecting size class.
inline void* allocate(RegionType /*region*/, size_t user_bytes, ActorId owner) {
    if (t_tla) {
        return t_tla->allocate_bytes(user_bytes, owner);
    }
    return std::malloc(user_bytes); // NOLINT: intentional fallback
}

// Allocate from a specific size class.
inline void* allocate_class(SizeClass sc, ActorId owner) {
    if (t_tla) {
        return t_tla->allocate(sc, owner);
    }
    return std::malloc(size_for_class(sc)); // NOLINT: intentional fallback
}

// Deallocate memory previously allocated via allocate() or allocate_class().
inline void deallocate(void* user_ptr) {
    if (!user_ptr) return;
    if (t_tla) {
        t_tla->deallocate(user_ptr);
        return;
    }
    std::free(user_ptr); // NOLINT: intentional fallback
}

} // namespace hpactor::mem
