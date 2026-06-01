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

#include <hpactor/mem/alloc_header.hpp>
#include <hpactor/mem/memory_config.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/mem/segment_provider.hpp>
#include <hpactor/mem/size_class.hpp>
#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/types/types.hpp>

#include <cstddef>
#include <cstdlib>
#include <memory>
#include <type_traits>

namespace hpactor::mem {

/// \brief std::allocator-compatible adapter for the HPActor slab allocator.
///
/// Stateful: stores an ActorId for per-actor memory tracking and a RegionType
/// for observability. All containers within an actor should use this allocator
/// to route allocations through the slab allocator instead of the global heap.
///
/// Two modes:
///   - Value mode:  \c MemStdAllocator(id(), RegionType::kActor) for local
///   vars.
///   - Pointer mode: \c MemStdAllocator(id_ptr(), RegionType::kActor) for actor
///     member containers — dereferences the pointer on each allocation so the
///     allocator tracks the live ActorId even when \c set_address() changes it.
///
/// Allocations <= 4 KB use the thread-local slab cache (bump + freelist).
/// Oversized allocations fall back to \c std::malloc / \c std::free.
///
/// \tparam T The element type to allocate.
/// \note Propagates on container copy/move/swap. Instances with different
///       owners are not equal — swapping containers with different owners
///       will reallocate.
template <typename T> class MemStdAllocator {
  public:
    using value_type = T;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    using is_always_equal = std::false_type;

    /// \brief Default constructor (owner is default ActorId).
    constexpr MemStdAllocator() noexcept = default;

    /// \brief Value-based owner constructor. Use for temporary/local
    /// containers.
    ///
    /// \param[in] owner The owning actor (copied by value).
    /// \param[in] region Memory region to charge allocations to (default
    /// kActor).
    explicit MemStdAllocator(ActorId owner,
                             RegionType region = RegionType::kActor) noexcept
        : owner_val_(owner), region_(region) {}

    /// \brief Pointer-based owner constructor. Use for actor member containers
    /// so the allocator tracks the live ActorId (which may change via
    /// \c set_address()).
    ///
    /// \param[in] owner_ptr Pointer to the actor's live ActorId.
    /// \param[in] region Memory region to charge allocations to (default
    /// kActor).
    explicit MemStdAllocator(const ActorId* owner_ptr,
                             RegionType region = RegionType::kActor) noexcept
        : owner_ptr_(owner_ptr), region_(region) {}

    /// \brief Rebind constructor (required by STL allocator concept).
    ///
    /// \tparam U Other element type.
    /// \param[in] other Allocator to copy owner and region from.
    template <typename U>
    MemStdAllocator(const MemStdAllocator<U>& other) noexcept
        : owner_ptr_(other.owner_ptr_), owner_val_(other.owner_val_),
          region_(other.region_) {}

    /// \brief Allocate memory for \p n elements of type T.
    ///
    /// Routes allocations <= 4 KB through the slab allocator; oversized
    /// allocations fall back to \c std::malloc.
    ///
    /// \param[in] n Number of elements to allocate.
    /// \return Pointer to uninitialized storage for \p n elements.
    /// \note Aborts via \c std::abort() if allocation fails.
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

        ActorId owner = (owner_ptr_ != nullptr) ? *owner_ptr_ : owner_val_;
        void* ptr = mem::allocate(region_, bytes, owner);
        if (!ptr) {
            std::abort();
        }
        return static_cast<T*>(ptr);
    }

    /// \brief Deallocate memory for \p n elements of type T.
    ///
    /// \param[in] ptr Pointer previously returned by \c allocate().
    /// \param[in] n Number of elements (must match the allocation count).
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

        mem::deallocate(ptr);
    }

    /// \brief Return the effective owner ActorId.
    ///
    /// Dereferences the owner pointer in pointer mode, or returns the stored
    /// value in value mode.
    ///
    /// \return The current ActorId.
    ActorId owner() const noexcept {
        return (owner_ptr_ != nullptr) ? *owner_ptr_ : owner_val_;
    }

    /// \brief Return the memory region charged by this allocator.
    ///
    /// \return The RegionType.
    RegionType region() const noexcept {
        return region_;
    }

    template <typename U> friend class MemStdAllocator;

  private:
    const ActorId* owner_ptr_{nullptr};
    ActorId owner_val_{};
    RegionType region_{RegionType::kActor};
};

/// \brief Equality comparison for MemStdAllocator.
///
/// Two allocators are equal if they have the same effective owner.
///
/// \tparam T Element type of the first allocator.
/// \tparam U Element type of the second allocator.
/// \param[in] a First allocator.
/// \param[in] b Second allocator.
/// \return \c true if both have the same owner.
template <typename T, typename U>
bool operator==(const MemStdAllocator<T>& a, const MemStdAllocator<U>& b) noexcept {
    return a.owner() == b.owner();
}

/// \brief Inequality comparison for MemStdAllocator.
///
/// \tparam T Element type of the first allocator.
/// \tparam U Element type of the second allocator.
/// \param[in] a First allocator.
/// \param[in] b Second allocator.
/// \return \c true if the allocators have different owners.
template <typename T, typename U>
bool operator!=(const MemStdAllocator<T>& a, const MemStdAllocator<U>& b) noexcept {
    return !(a == b);
}

// =========================================================================
// MemDeleter — custom deleter for std::unique_ptr that routes deallocation
// through the slab allocator instead of ::operator delete.
// =========================================================================

/// \brief Custom deleter for \c std::unique_ptr that routes deallocation
/// through the slab allocator.
///
/// Calls the destructor and then \c mem::deallocate() for the raw storage.
///
/// \tparam T The managed object type.
template <typename T> struct MemDeleter {
    /// \brief Destroy and deallocate a slab-allocated object.
    ///
    /// \param[in] ptr Pointer to the object (must have been allocated via the
    ///            slab allocator).
    void operator()(T* ptr) const noexcept {
        if (ptr) {
            ptr->~T();
            deallocate(ptr);
        }
    }
};

/// \brief \c std::unique_ptr alias using slab deallocation.
///
/// \tparam T The managed object type.
template <typename T> using MemUniquePtr = std::unique_ptr<T, MemDeleter<T>>;

// =========================================================================
// allocate_shared — std::allocate_shared wrapper with MemStdAllocator.
// Same return type as std::make_shared (no cascading type changes).
// =========================================================================

/// \brief Create a \c std::shared_ptr with slab-allocated control block and
/// object (value-based owner).
///
/// \tparam T Object type to construct.
/// \tparam Args Constructor argument types.
/// \param[in] owner The owning actor (by value).
/// \param[in] region Memory region to charge.
/// \param[in] args Constructor arguments forwarded to T.
/// \return A \c std::shared_ptr<T> (same return type as \c std::make_shared).
template <typename T, typename... Args>
std::shared_ptr<T>
allocate_shared(ActorId owner, RegionType region, Args&&... args) {
    return std::allocate_shared<T>(MemStdAllocator<T>(owner, region),
                                   std::forward<Args>(args)...);
}

/// \brief Create a \c std::shared_ptr with slab-allocated control block and
/// object (pointer-based owner).
///
/// Tracks the live ActorId for container members whose owner may change.
///
/// \tparam T Object type to construct.
/// \tparam Args Constructor argument types.
/// \param[in] owner_ptr Pointer to the live ActorId.
/// \param[in] region Memory region to charge.
/// \param[in] args Constructor arguments forwarded to T.
/// \return A \c std::shared_ptr<T>.
template <typename T, typename... Args>
std::shared_ptr<T>
allocate_shared(const ActorId* owner_ptr, RegionType region, Args&&... args) {
    return std::allocate_shared<T>(MemStdAllocator<T>(owner_ptr, region),
                                   std::forward<Args>(args)...);
}

// =========================================================================
// allocate_unique — placement-new + slab allocation + MemDeleter.
// Returns std::unique_ptr<T, MemDeleter<T>> (type differs from make_unique).
// =========================================================================

/// \brief Create a \c MemUniquePtr<T> with slab-allocated storage (value-based
/// owner).
///
/// \tparam T Object type to construct.
/// \tparam Args Constructor argument types.
/// \param[in] owner The owning actor (by value).
/// \param[in] region Memory region to charge.
/// \param[in] args Constructor arguments forwarded to T.
/// \return A \c MemUniquePtr<T> owning the slab-allocated object.
/// \note Aborts via \c std::abort() if allocation fails.
template <typename T, typename... Args>
MemUniquePtr<T> allocate_unique(ActorId owner, RegionType region, Args&&... args) {
    void* mem = allocate(region, sizeof(T), owner);
    if (!mem) {
        std::abort();
    }
    // NOLINTNEXTLINE(cppcoreguidelines-owning-memory)
    return MemUniquePtr<T>(::new (mem) T(std::forward<Args>(args)...));
}

/// \brief Create a \c MemUniquePtr<T> with slab-allocated storage
/// (pointer-based owner).
///
/// \tparam T Object type to construct.
/// \tparam Args Constructor argument types.
/// \param[in] owner_ptr Pointer to the live ActorId.
/// \param[in] region Memory region to charge.
/// \param[in] args Constructor arguments forwarded to T.
/// \return A \c MemUniquePtr<T> owning the slab-allocated object.
template <typename T, typename... Args>
MemUniquePtr<T>
allocate_unique(const ActorId* owner_ptr, RegionType region, Args&&... args) {
    ActorId owner = (owner_ptr != nullptr) ? *owner_ptr : ActorId{};
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

/// \brief CRTP base that overrides \c operator \c new / \c operator \c delete
/// to route through the slab allocator.
///
/// Inherit from this to make \c std::make_unique (and any other new-expression)
/// automatically use the slab allocator. \c operator \c delete safely handles
/// cross-thread frees by falling back to slab lookup when the thread-local
/// allocator pointer is unavailable.
///
/// \tparam Derived The derived class (CRTP parameter).
/// \pre \c t_tla and \c t_current_actor_id must be set on the calling thread.
///      The scheduler guarantees this before dispatching actor work.
/// \note Array new/delete are explicitly deleted — only scalar allocation
///       is supported.
template <typename Derived> class SlabAllocated {
  public:
    /// \brief Allocate storage for a Derived object via the slab allocator.
    ///
    /// Falls back to \c ::operator new if the thread-local allocator is not
    /// set.
    ///
    /// \param[in] size Size of the object in bytes.
    /// \return Pointer to raw storage.
    // NOLINTNEXTLINE(cppcoreguidelines-no-malloc)
    static void* operator new(size_t size) {
        void* ptr = nullptr;
        if (t_tla) {
            ptr = t_tla->allocate_bytes(size, current_actor_id());
        }
        if (!ptr) {
            ptr = ::operator new(size); // NOLINT
        }
        return ptr;
    }

    /// \brief Deallocate a Derived object.
    ///
    /// When \c t_tla is available on the calling thread, routes through it.
    /// Otherwise performs a slab lookup via SegmentProvider to route the free
    /// to the origin SlabCache (handles cross-thread frees).
    ///
    /// \param[in] ptr Pointer to the object to deallocate.
    static void operator delete(void* ptr) noexcept {
        if (!ptr)
            return;
        if (t_tla) {
            t_tla->deallocate(ptr);
            return;
        }
        auto* header = AllocHeader::from_user_data(ptr);
        auto slab = SegmentProvider::instance().lookup_slab(header);
        if (slab.found && slab.owner_cache) {
            static_cast<SlabCache*>(slab.owner_cache)->deallocate(ptr);
        } else {
            ::operator delete(ptr); // NOLINT
        }
    }

    /// \brief Array new — deleted (not supported).
    static void* operator new[](size_t size) = delete;
    /// \brief Array delete — deleted (not supported).
    static void operator delete[](void* ptr) = delete;

  protected:
    /// \brief Protected destructor — only derived classes may destroy.
    ~SlabAllocated() = default;
};

} // namespace hpactor::mem
