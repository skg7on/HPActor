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

namespace hpactor::adt {

// Lock-free LIFO freelist with ABA protection via tagged pointers.
// Each node must have a `T* next` field.
// Uses 128-bit CAS (pointer + counter) to prevent the ABA problem.
// Thread-safe for any number of concurrent push/pop operations.
template <typename T> class FreeList {
  public:
    FreeList() {
        top_.store({nullptr, 0}, std::memory_order_relaxed);
    }

    void push(T* node) noexcept {
        TaggedPtr old = top_.load(std::memory_order_acquire);
        TaggedPtr desired;
        do {
            node->next = old.ptr;
            desired.ptr = node;
            desired.tag = old.tag + 1;
        } while (!top_.compare_exchange_weak(
            old, desired, std::memory_order_acq_rel, std::memory_order_acquire));
    }

    T* pop() noexcept {
        TaggedPtr old = top_.load(std::memory_order_acquire);
        while (old.ptr != nullptr) {
            TaggedPtr desired;
            desired.ptr = old.ptr->next;
            desired.tag = old.tag + 1;
            if (top_.compare_exchange_weak(old, desired, std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
                return old.ptr;
            }
        }
        return nullptr;
    }

    bool empty() const noexcept {
        return top_.load(std::memory_order_acquire).ptr == nullptr;
    }

  private:
    struct alignas(16) TaggedPtr {
        T* ptr;
        uintptr_t tag;
    };
    std::atomic<TaggedPtr> top_;
};

} // namespace hpactor::adt
