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
#include <memory>
#include <vector>

namespace hpactor::adt {

// Fixed-size pool for coroutine stack frames.
// Uses a lock-free stack for O(1) acquire and release.
class CoroutineFramePool {
  public:
    struct Frame {
        std::byte* stack_ptr;
        size_t stack_size;
        bool in_use{false};
    };

    explicit CoroutineFramePool(size_t num_frames, size_t stack_size = 8 * 1024);

    ~CoroutineFramePool();

    CoroutineFramePool(const CoroutineFramePool&) = delete;
    CoroutineFramePool& operator=(const CoroutineFramePool&) = delete;
    CoroutineFramePool(CoroutineFramePool&&) = delete;
    CoroutineFramePool& operator=(CoroutineFramePool&&) = delete;

    Frame* acquire();
    void release(Frame* frame);

    bool empty() const {
        return free_count_.load(std::memory_order_acquire) == 0;
    }

    size_t available() const {
        return free_count_.load(std::memory_order_acquire);
    }

    size_t total() const {
        return frames_.size();
    }

    size_t stack_size() const {
        return stack_size_;
    }

  private:
    struct FreeNode {
        FreeNode* next;
        size_t index;
    };

    std::atomic<FreeNode*> free_stack_{nullptr};
    std::atomic<size_t> free_count_{0};
    std::vector<std::unique_ptr<std::byte[]>> stacks_;
    std::vector<Frame> frames_;
    size_t stack_size_;
};

} // namespace hpactor::adt
