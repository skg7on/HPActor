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

#include "hpactor/log/log_event.hpp"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>

namespace hpactor::log {

class LogRingBuffer {
  public:
    explicit LogRingBuffer(size_t capacity)
        : capacity_(capacity), mask_(capacity - 1) {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            std::fprintf(stderr,
                         "LogRingBuffer capacity must be a power of two, got "
                         "%zu\n",
                         capacity);
            std::abort();
        }
        buffer_ = std::make_unique<LogEvent[]>(capacity);
    }

    bool try_push(const LogEvent& value) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        uint64_t r = read_cursor_.load(std::memory_order_acquire);

        if (w - r >= capacity_) {
            events_lost_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        while (!write_cursor_.compare_exchange_weak(
            w, w + 1, std::memory_order_acquire, std::memory_order_relaxed)) {
            r = read_cursor_.load(std::memory_order_acquire);
            if (w - r >= capacity_) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        buffer_[w & mask_] = value;
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    template <typename Fn> size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);

        size_t count = 0;
        while (r < w) {
            callback(buffer_[r & mask_]);
            ++r;
            ++count;
        }

        read_cursor_.store(r, std::memory_order_release);
        return count;
    }

    uint64_t events_lost() const noexcept {
        return events_lost_.load(std::memory_order_relaxed);
    }

    size_t size() const noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        uint64_t r = read_cursor_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept {
        return size() == 0;
    }

  private:
    size_t capacity_;
    size_t mask_;
    std::unique_ptr<LogEvent[]> buffer_;

    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
};

} // namespace hpactor::log
