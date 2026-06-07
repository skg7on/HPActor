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
#include <chrono>
#include <cstdint>
#include <functional>
#include <thread>

namespace hpactor {

class ActorSystem;

/// \brief Monitors system memory and triggers passivation of idle actors
///        under pressure.
///
/// Polls at a configurable interval. When memory usage exceeds the high
/// watermark, invokes a callback so the actor registry can select LRU
/// passivatable actors.
class MemoryPressureMonitor {
  public:
    /// \brief Configuration for the monitor.
    struct Config {
        bool enabled = true;
        uint8_t high_threshold_pct = 85;
        std::chrono::milliseconds poll_interval{5000};
    };

    /// \brief Callback type: invoked when memory pressure is high.
    using Callback = std::function<void()>;

    MemoryPressureMonitor(Config config, Callback cb);
    ~MemoryPressureMonitor();

    MemoryPressureMonitor(const MemoryPressureMonitor&) = delete;
    MemoryPressureMonitor& operator=(const MemoryPressureMonitor&) = delete;

    bool is_running() const noexcept {
        return running_.load(std::memory_order_acquire);
    }
    void stop();

  private:
    void poll_loop();
    uint8_t current_pressure_pct() const;

    Config config_;
    Callback callback_;
    std::atomic<bool> running_{true};
    std::thread poll_thread_;
};

} // namespace hpactor
