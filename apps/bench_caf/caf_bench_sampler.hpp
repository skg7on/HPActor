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
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#    include <mach/mach.h>
#endif

namespace hpactor::apps::bench_caf {

inline uint64_t sample_current_rss_bytes() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (status >> key >> value >> unit) {
        if (key == "VmRSS:")
            return value * 1024;
    }
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    auto result = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                            reinterpret_cast<task_info_t>(&info), &count);
    if (result != KERN_SUCCESS)
        return 0;
    return static_cast<uint64_t>(info.resident_size);
#else
    return 0;
#endif
}

class RssSampler {
  public:
    explicit RssSampler(uint32_t interval_ms)
        : interval_ms_(interval_ms == 0 ? 50 : interval_ms) {}

    ~RssSampler() {
        stop();
    }

    void start() {
        // Join any previous worker to prevent concurrent access to samples_
        // if start() is called twice without an intervening stop().
        if (worker_.joinable())
            worker_.join();
        running_.store(true, std::memory_order_release);
        samples_.clear();
        worker_ = std::thread([this]() {
            while (running_.load(std::memory_order_acquire)) {
                samples_.push_back(sample_current_rss_bytes());
                std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
            }
            samples_.push_back(sample_current_rss_bytes());
        });
    }

    std::vector<uint64_t> stop() {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
        return samples_;
    }

  private:
    uint32_t interval_ms_ = 50;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::vector<uint64_t> samples_;
};

} // namespace hpactor::apps::bench_caf
