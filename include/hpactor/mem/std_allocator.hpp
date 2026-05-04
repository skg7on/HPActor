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
#include <memory>
#include <type_traits>

namespace hpactor::mem {

/// std::allocator-compatible adapter for the HPActor slab allocator.
///
/// Stateful: stores an ActorId for per-actor memory tracking and a RegionType
/// for observability. All containers within an actor should use this allocator
/// to route allocations through the slab allocator instead of the global heap.
///
/// Two modes:
///   - Value mode:  MemStdAllocator(id(), RegionType::kActor) for local vars
///   - Pointer mode: MemStdAllocator(id_ptr(), RegionType::kActor) for actor
///     member containers — dereferences the pointer on each allocation so the
///     allocator tracks the live ActorId even when set_address() changes it.
///
/// Allocations <= 4KB use the thread-local slab cache (bump + freelist).
/// Oversized allocations fall back to std::malloc / std::free.
template <typename T>
class MemStdAllocator {
  public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    constexpr MemStdAllocator() noexcept = default;

    /// Value-based owner. Use for temporary/local containers.
    explicit MemStdAllocator(ActorId owner,
                             RegionType region = RegionType::kActor) noexcept
        : owner_val_(owner), region_(region) {}

    /// Pointer-based owner. Use for actor member containers so the allocator
    /// tracks the live ActorId (which may change via set_address).
    explicit MemStdAllocator(const ActorId* owner_ptr,
                             RegionType region = RegionType::kActor) noexcept
        : owner_ptr_(owner_ptr), region_(region) {}

    template <typename U>
    MemStdAllocator(const MemStdAllocator<U>& other) noexcept
        : owner_ptr_(other.owner_ptr_), owner_val_(other.owner_val_),
          region_(other.region_) {}

    [[nodiscard]] T* allocate(size_t n) {
        if (n == 0) {
            return nullptr;
        }
        size_t bytes = n * sizeof(T);

        if (bytes > size_for_class(SizeClass::k4KB)) {
            // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
            void* ptr = std::malloc(bytes);
            if (!ptr) {
                std::abort();
            }
            return static_cast<T*>(ptr);
        }

        void* ptr = nullptr;
        ActorId owner = (owner_ptr_ != nullptr) ? *owner_ptr_ : owner_val_;
        if (t_tla) {
            ptr = t_tla->allocate_bytes(bytes, owner);
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

    ActorId owner() const noexcept {
        return (owner_ptr_ != nullptr) ? *owner_ptr_ : owner_val_;
    }
    RegionType region() const noexcept { return region_; }

    template <typename U>
    friend class MemStdAllocator;

  private:
    const ActorId* owner_ptr_{nullptr};
    ActorId owner_val_{};
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

// =========================================================================
// MemDeleter — custom deleter for std::unique_ptr that routes deallocation
// through the slab allocator instead of ::operator delete.
// =========================================================================
template <typename T>
struct MemDeleter {
    void operator()(T* ptr) const noexcept {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }
};

// Unique pointer alias using slab deallocation.
template <typename T>
using MemUniquePtr = std::unique_ptr<T, MemDeleter<T>>;

// =========================================================================
// allocate_shared — std::allocate_shared wrapper with MemStdAllocator.
// Same return type as std::make_shared (no cascading type changes).
// =========================================================================

/// Value-based owner.
template <typename T, typename... Args>
std::shared_ptr<T> allocate_shared(ActorId owner, RegionType region,
                                   Args&&... args) {
    return std::allocate_shared<T>(
        MemStdAllocator<T>(owner, region),
        std::forward<Args>(args)...);
}

/// Pointer-based owner — tracks the live ActorId (for actor members).
template <typename T, typename... Args>
std::shared_ptr<T> allocate_shared(const ActorId* owner_ptr,
                                   RegionType region, Args&&... args) {
    return std::allocate_shared<T>(
        MemStdAllocator<T>(owner_ptr, region),
        std::forward<Args>(args)...);
}

// =========================================================================
// allocate_unique — placement-new + slab allocation + MemDeleter.
// Returns std::unique_ptr<T, MemDeleter<T>> (type differs from make_unique).
// =========================================================================

/// Value-based owner.
template <typename T, typename... Args>
MemUniquePtr<T> allocate_unique(ActorId owner, RegionType region,
                                Args&&... args) {
    void* mem = allocate(region, sizeof(T), owner);
    if (!mem) {
        std::abort();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return MemUniquePtr<T>(::new (mem) T(std::forward<Args>(args)...));
}

/// Pointer-based owner — tracks the live ActorId.
template <typename T, typename... Args>
MemUniquePtr<T> allocate_unique(const ActorId* owner_ptr,
                                RegionType region, Args&&... args) {
    ActorId owner =
        (owner_ptr != nullptr) ? *owner_ptr : ActorId{};
    return allocate_unique<T>(owner, region, std::forward<Args>(args)...);
}

// =========================================================================
// SlabAllocated — CRTP base that overrides operator new/delete to route
// through the slab allocator. Inherit from this to make std::make_unique
// (and any other new-expression) use the slab allocator automatically.
//
// Requires t_tla and t_current_actor_id to be set on the calling thread
// (the scheduler does this before dispatching actor work).
// =========================================================================
template <typename Derived>
class SlabAllocated {
  public:
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    static void* operator new(size_t size) {
        void* ptr = nullptr;
        if (t_tla) {
            ptr = t_tla->allocate_bytes(size, current_actor_id());
        }
        if (!ptr) {
            ptr = ::operator new(size);  // NOLINT
        }
        return ptr;
    }

    static void operator delete(void* ptr) noexcept {
        if (!ptr) return;
        if (t_tla) {
            t_tla->deallocate(ptr);
        } else {
            ::operator delete(ptr);  // NOLINT
        }
    }

    // Array new/delete — required by C++ for completeness.
    static void* operator new[](size_t size) = delete;
    static void operator delete[](void* ptr) = delete;

  protected:
    ~SlabAllocated() = default;
};

} // namespace hpactor::mem
