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

#include <hpactor/sched/coroutine_frame_pool.hpp>

namespace hpactor::sched {

CoroutineFramePool::CoroutineFramePool(size_t num_frames, size_t stack_size)
    : frames_(num_frames), stack_size_(stack_size) {
    // Pre-allocate all stacks and build the free list
    stacks_.reserve(num_frames);

    for (size_t i = 0; i < num_frames; ++i) {
        // Allocate stack memory
        auto* stack = new std::byte[stack_size];
        stacks_.emplace_back(stack);

        // Initialize frame
        frames_[i].stack_ptr = stack;
        frames_[i].stack_size = stack_size;
        frames_[i].in_use = false;

        // Push onto free stack with frame index
        auto* node = reinterpret_cast<FreeNode*>(stack);
        node->next = free_stack_.load(std::memory_order_relaxed);
        node->index = i;
        free_stack_.store(node, std::memory_order_release);
    }

    free_count_.store(num_frames, std::memory_order_release);
}

CoroutineFramePool::~CoroutineFramePool() = default;

CoroutineFramePool::Frame* CoroutineFramePool::acquire() {
    // Pop from free stack
    FreeNode* node = free_stack_.load(std::memory_order_acquire);
    while (node != nullptr) {
        FreeNode* next = node->next;
        if (free_stack_.compare_exchange_weak(node, next, std::memory_order_acq_rel,
                                              std::memory_order_acquire)) {
            free_count_.fetch_sub(1, std::memory_order_release);

            // Retrieve frame directly from stored index
            size_t index = node->index;
            Frame* frame = &frames_[index];
            frame->in_use = true;
            return frame;
        }
        // CAS failed, node was updated, retry
    }
    return nullptr; // Pool exhausted
}

void CoroutineFramePool::release(Frame* frame) {
    if (!frame || !frame->in_use) {
        return;
    }

    frame->in_use = false;

    // Push onto free stack (restore index first, since the stack region may
    // have been written to)
    size_t index = static_cast<size_t>(frame - frames_.data());
    auto* node = reinterpret_cast<FreeNode*>(frame->stack_ptr);
    node->index = index;
    FreeNode* current = free_stack_.load(std::memory_order_acquire);
    do {
        node->next = current;
    } while (!free_stack_.compare_exchange_weak(
        current, node, std::memory_order_acq_rel, std::memory_order_acquire));

    free_count_.fetch_add(1, std::memory_order_release);
}

} // namespace hpactor::sched