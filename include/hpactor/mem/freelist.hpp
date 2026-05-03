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

namespace hpactor::mem {

// Lock-free LIFO freelist. Each node must have a `T* next` field.
// Thread-safe for any number of concurrent push/pop operations.
template <typename T> class FreeList {
  public:
    FreeList() : top_(nullptr) {}

    void push(T* node) noexcept {
        node->next = top_.load(std::memory_order_acquire);
        while (!top_.compare_exchange_weak(node->next, node,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
        }
    }

    T* pop() noexcept {
        T* node = top_.load(std::memory_order_acquire);
        while (node != nullptr) {
            T* next = node->next;
            if (top_.compare_exchange_weak(node, next,
                                           std::memory_order_acq_rel,
                                           std::memory_order_acquire)) {
                return node;
            }
        }
        return nullptr;
    }

    bool empty() const noexcept {
        return top_.load(std::memory_order_acquire) == nullptr;
    }

  private:
    std::atomic<T*> top_;
};

} // namespace hpactor::mem
