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

#include <atomic>
#include <cassert>
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_ring_buffer.hpp>
#include <thread>
#include <vector>

using namespace hpactor::log;

static LogEvent make_event() {
    LogEvent evt{};
    evt.level = LogLevel::kInfo;
    evt.category = LogCategory::kUser;
    evt.message = "test";
    return evt;
}

int main() {
    // Test: Push and Drain
    {
        LogRingBuffer rb(64);
        assert(rb.empty());
        assert(rb.size() == 0);

        auto evt = make_event();
        assert(rb.try_push(evt));
        assert(!rb.empty());
        assert(rb.size() == 1);

        int count = 0;
        rb.drain([&](const LogEvent& e) {
            assert(e.level == LogLevel::kInfo);
            count++;
        });
        assert(count == 1);
        assert(rb.empty());
    }

    // Test: Overflow drops and counts
    {
        LogRingBuffer rb(4);
        LogEvent evt{};

        for (int i = 0; i < 4; i++) {
            assert(rb.try_push(evt));
        }
        assert(!rb.try_push(evt));
        assert(rb.events_lost() > 0);
    }

    // Test: Concurrent producers
    {
        LogRingBuffer rb(1024);
        std::atomic<int> total_pushed{0};
        std::atomic<int> total_dropped{0};

        auto producer = [&]() {
            LogEvent evt{};
            for (int i = 0; i < 10000; i++) {
                if (rb.try_push(evt)) {
                    total_pushed.fetch_add(1, std::memory_order_relaxed);
                } else {
                    total_dropped.fetch_add(1, std::memory_order_relaxed);
                }
            }
        };

        std::thread t1(producer);
        std::thread t2(producer);
        std::thread t3(producer);
        t1.join();
        t2.join();
        t3.join();

        int drained = 0;
        rb.drain([&](const LogEvent&) { drained++; });

        assert(total_pushed.load() == drained);
        assert(rb.events_lost() == static_cast<uint64_t>(total_dropped.load()));
    }

    // Test: Drain after overflow doesn't lose prior events
    {
        LogRingBuffer rb(8);
        LogEvent evt{};
        evt.message = "keep";

        // Fill
        for (int i = 0; i < 8; i++) {
            assert(rb.try_push(evt));
        }
        // Overflow
        assert(!rb.try_push(evt));
        assert(rb.events_lost() == 1);

        // Drain should get exactly 8 events
        int drained = 0;
        rb.drain([&](const LogEvent&) { drained++; });
        assert(drained == 8);
        assert(rb.empty());
    }

    return 0;
}
