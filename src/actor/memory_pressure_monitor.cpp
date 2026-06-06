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

#include <hpactor/actor/memory_pressure_monitor.hpp>

#if defined(__APPLE__)
#    include <mach/mach.h>
#elif defined(__linux__)
#    include <sys/sysinfo.h>
#endif

namespace hpactor {

MemoryPressureMonitor::MemoryPressureMonitor(Config config, Callback cb)
    : config_(std::move(config)), callback_(std::move(cb)) {
    if (config_.enabled) {
        poll_thread_ = std::thread(&MemoryPressureMonitor::poll_loop, this);
    }
}

MemoryPressureMonitor::~MemoryPressureMonitor() {
    stop();
}

void MemoryPressureMonitor::stop() {
    running_.store(false, std::memory_order_release);
    if (poll_thread_.joinable()) {
        poll_thread_.join();
    }
}

uint8_t MemoryPressureMonitor::current_pressure_pct() const {
#if defined(__APPLE__)
    mach_port_t host = mach_host_self();
    vm_statistics64_data_t vm_stats;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(host, HOST_VM_INFO64,
                          reinterpret_cast<host_info64_t>(&vm_stats),
                          &count) != KERN_SUCCESS) {
        return 0;
    }
    uint64_t total = vm_stats.wire_count + vm_stats.active_count +
                     vm_stats.inactive_count + vm_stats.free_count;
    uint64_t used = vm_stats.wire_count + vm_stats.active_count;
    if (total == 0) {
        return 0;
    }
    return static_cast<uint8_t>((used * 100) / total);
#elif defined(__linux__)
    struct sysinfo si;
    if (sysinfo(&si) != 0) {
        return 0;
    }
    uint64_t total = si.totalram;
    uint64_t used = total - si.freeram;
    if (total == 0) {
        return 0;
    }
    return static_cast<uint8_t>((used * 100) / total);
#else
    return 0;
#endif
}

void MemoryPressureMonitor::poll_loop() {
    while (running_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(config_.poll_interval);
        if (!running_.load(std::memory_order_acquire)) {
            break;
        }

        uint8_t pressure = current_pressure_pct();
        if (pressure >= config_.high_threshold_pct) {
            if (callback_) {
                callback_();
            }
        }
    }
}

} // namespace hpactor
