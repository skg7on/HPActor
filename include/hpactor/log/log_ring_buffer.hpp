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

#include <hpactor/adt/mpsc_ring_buffer.hpp>
#include <hpactor/log/log_event.hpp>

namespace hpactor::log {

// Thin wrapper around adt::DynamicMpscRingBuffer<LogEvent>.
// Preserves the LogRingBuffer type identity for existing callers
// (LogManager, LogDrain, Logger, test_log_ring_buffer).
class LogRingBuffer {
  public:
    explicit LogRingBuffer(size_t capacity) : buffer_(capacity) {}

    bool try_push(const LogEvent& value) noexcept {
        return buffer_.try_push(value);
    }

    template <typename Fn> size_t drain(Fn&& callback) {
        return buffer_.drain(std::forward<Fn>(callback));
    }

    uint64_t events_lost() const noexcept {
        return buffer_.events_lost();
    }

    size_t size() const noexcept {
        return buffer_.size();
    }

    bool empty() const noexcept {
        return buffer_.empty();
    }

  private:
    adt::DynamicMpscRingBuffer<LogEvent> buffer_;
};

} // namespace hpactor::log
