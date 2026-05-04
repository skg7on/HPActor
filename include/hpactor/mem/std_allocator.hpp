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

#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/types/types.hpp>

#include <cstddef>
#include <cstdlib>
#include <type_traits>

namespace hpactor::mem {

/// std::allocator-compatible adapter for the HPActor slab allocator.
///
/// Stateful: stores an ActorId for per-actor memory tracking and a RegionType
/// for observability. All containers within an actor should use this allocator
/// to route allocations through the slab allocator instead of the global heap.
///
/// Allocations <= 4KB use the thread-local slab cache (bump + freelist).
/// Oversized allocations fall back to std::malloc / std::free.
///
/// Usage from within an actor:
///   std::vector<int, MemStdAllocator<int>> vec(
///       MemStdAllocator<int>(id(), RegionType::kActor));
template <typename T>
class MemStdAllocator {
  public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    // Allocator propagation traits: when a container is copied, moved, or
    // swapped, the allocator travels with it so allocations stay attributed
    // to the correct actor.
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;

    // Not always equal — allocators with different owners must not be
    // considered interchangeable, otherwise STL containers would swap/move
    // internal buffers between actors, corrupting per-actor tracking.
    using is_always_equal = std::false_type;

    constexpr MemStdAllocator() noexcept = default;

    explicit MemStdAllocator(ActorId owner,
                             RegionType region = RegionType::kActor) noexcept
        : owner_(owner), region_(region) {}

    template <typename U>
    constexpr MemStdAllocator(const MemStdAllocator<U>& other) noexcept
        : owner_(other.owner()), region_(other.region()) {}

    [[nodiscard]] T* allocate(size_t n) {
        if (n == 0) {
            return nullptr;
        }
        size_t bytes = n * sizeof(T);

        // Oversized — exceeds the largest slab size class, use malloc
        if (bytes > size_for_class(SizeClass::k4KB)) {
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            void* ptr = std::malloc(bytes);
            if (!ptr) {
                std::abort();
            }
            return static_cast<T*>(ptr);
        }

        void* ptr = nullptr;
        if (t_tla) {
            ptr = t_tla->allocate_bytes(bytes, owner_);
        } else {
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            ptr = std::malloc(bytes);
        }
        if (!ptr) {
            std::abort();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, size_t n) noexcept {
        if (!ptr || n == 0) {
            return;
        }
        size_t bytes = n * sizeof(T);

        if (bytes > size_for_class(SizeClass::k4KB)) {
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            std::free(ptr);
            return;
        }

        if (t_tla) {
            t_tla->deallocate(ptr);
        } else {
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            std::free(ptr);
        }
    }

    ActorId owner() const noexcept { return owner_; }
    RegionType region() const noexcept { return region_; }

    template <typename U>
    friend class MemStdAllocator;

  private:
    ActorId owner_{};
    RegionType region_{RegionType::kActor};
};

template <typename T, typename U>
bool operator==(const MemStdAllocator<T>& a,
                const MemStdAllocator<U>& b) noexcept {
    return a.owner() == b.owner();
}

template <typename T, typename U>
bool operator!=(const MemStdAllocator<T>& a,
                const MemStdAllocator<U>& b) noexcept {
    return !(a == b);
}

} // namespace hpactor::mem
