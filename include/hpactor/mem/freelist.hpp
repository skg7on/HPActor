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
#include <cstddef>
#include <cstdint>

namespace hpactor::mem {

/// \brief Lock-free LIFO freelist with ABA protection via 64-bit tagged
/// pointers.
///
/// Packs a 48-bit pointer with a 16-bit ABA counter into a single \c uint64_t,
/// making the CAS always lock-free on 64-bit platforms.
///
/// Each node must have a \c T* \c next field.
///
/// \tparam T Node type with a \c T* \c next member.
/// \note Thread-safe: any number of concurrent \c push() and \c pop()
/// operations
///       are lock-free and linearizable.
template <typename T> class FreeList {
  public:
    /// \brief Construct an empty freelist.
    FreeList() {
        top_.store(0, std::memory_order_relaxed);
    }

    /// \brief Push a node onto the freelist.
    ///
    /// \param[in] node Node to push. Must have a valid \c next field.
    /// \note Lock-free; safe to call from any thread.
    void push(T* node) noexcept {
        uint64_t old = top_.load(std::memory_order_acquire);
        uint64_t desired;
        do {
            node->next = extract_ptr(old);
            desired = pack(node, next_tag(old));
        } while (!top_.compare_exchange_weak(
            old, desired, std::memory_order_acq_rel, std::memory_order_acquire));
    }

    /// \brief Pop a node from the freelist.
    ///
    /// \return The popped node, or \c nullptr if the freelist is empty.
    /// \note Lock-free; safe to call from any thread.
    T* pop() noexcept {
        uint64_t old = top_.load(std::memory_order_acquire);
        while (extract_ptr(old) != nullptr) {
            uint64_t desired = pack(extract_ptr(old)->next, next_tag(old));
            if (top_.compare_exchange_weak(old, desired, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
                return extract_ptr(old);
            }
        }
        return nullptr;
    }

    /// \brief Check whether the freelist is empty.
    ///
    /// \return \c true if no nodes are currently linked.
    /// \note The result is a snapshot — a concurrent push/pop may change state
    ///       before the caller acts on the return value.
    bool empty() const noexcept {
        return extract_ptr(top_.load(std::memory_order_acquire)) == nullptr;
    }

  private:
    // User-space virtual addresses on x86-64 and ARM64 use ≤ 48 bits.
    // The upper 16 bits carry the ABA counter.
    static constexpr uint64_t kPtrMask = (1ULL << 48) - 1;
    static constexpr int kTagShift = 48;

    /// \brief Extract the pointer from a packed tagged-pointer word, with
    /// sign-extension for canonical addresses above the 47-bit boundary.
    static T* extract_ptr(uint64_t packed) noexcept {
        constexpr uint64_t kSignBit = 1ULL << 47;
        uint64_t ptr = packed & kPtrMask;
        if (ptr & kSignBit) {
            ptr |= ~kPtrMask;
        }
        return reinterpret_cast<T*>(static_cast<uintptr_t>(ptr));
    }

    /// \brief Increment and return the ABA tag from a packed word.
    static uint16_t next_tag(uint64_t packed) noexcept {
        return static_cast<uint16_t>((packed >> kTagShift) + 1);
    }

    /// \brief Pack a pointer and tag into a single 64-bit word.
    static uint64_t pack(T* ptr, uint16_t tag) noexcept {
        return (reinterpret_cast<uint64_t>(ptr) & kPtrMask) |
               (static_cast<uint64_t>(tag) << kTagShift);
    }

    std::atomic<uint64_t> top_;
};

} // namespace hpactor::mem
