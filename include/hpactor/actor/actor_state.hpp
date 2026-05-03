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
#include <cstdint>

namespace hpactor {

// ActorState: atomic state encoding for actor lifecycle
// States: Idle(0) → Ready(1) → Running(2) → IOWaiting(3) / Terminated(4)
class ActorState {
  public:
    static constexpr uint32_t kIdle = 0x01;
    static constexpr uint32_t kReady = 0x02;
    static constexpr uint32_t kRunning = 0x04;
    static constexpr uint32_t kIOWaiting = 0x08;
    static constexpr uint32_t kTerminated = 0x10;
    static constexpr uint32_t kHibernating = 0x20;
    static constexpr uint32_t kMask = 0x3F;

    ActorState() : state_(kIdle) {}
    explicit ActorState(uint32_t initial) : state_(initial) {}

    uint32_t get() const {
        return state_.load(std::memory_order_acquire);
    }

    // CAS transition. Returns true if successful.
    bool cas(uint32_t expected, uint32_t desired) {
        return state_.compare_exchange_strong(expected, desired,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire);
    }

    void set(uint32_t s) {
        state_.store(s, std::memory_order_release);
    }

    bool is_idle() const {
        return get() == kIdle;
    }
    bool is_ready() const {
        return get() == kReady;
    }
    bool is_running() const {
        return get() == kRunning;
    }
    bool is_io_waiting() const {
        return get() == kIOWaiting;
    }
    bool is_terminated() const {
        return get() == kTerminated;
    }
    bool is_hibernating() const {
        return get() == kHibernating;
    }

  private:
    std::atomic<uint32_t> state_;
};

} // namespace hpactor
