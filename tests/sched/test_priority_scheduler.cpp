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

// =============================================================================
// Test: Priority Scheduler Interface
// =============================================================================

#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>
#include <chrono>
#include <iostream>

using namespace hpactor;

int main() {
    std::cout << "=== Priority Scheduler Tests ===" << std::endl;

    // Test 1: Scheduler creation
    std::cout << "Test: Scheduler creation..." << std::endl;
    {
        Config config{.scheduler_threads = 4, .max_queue_depth = 1024};
        ActorSystem system(config);
        assert(system.scheduler() != nullptr);
        assert(system.scheduler()->worker_count() == 4);
        assert(system.scheduler()->is_running());
        std::cout << "  PASS: Scheduler created with 4 workers" << std::endl;
    }
    std::cout << "  ActorSystem destroyed cleanly" << std::endl;

    // Test 2: notify_ready with priorities
    std::cout << "Test: notify_ready with priorities..." << std::endl;
    {
        Config config{.scheduler_threads = 2, .max_queue_depth = 1024};
        ActorSystem system(config);

        ActorId actors[] = {ActorId{1}, ActorId{2}, ActorId{3}, ActorId{4}};

        system.scheduler()->notify_ready(actors[0], 3, INT64_MAX);
        system.scheduler()->notify_ready(actors[1], 0, INT64_MAX);
        system.scheduler()->notify_ready(actors[2], 1, INT64_MAX);
        system.scheduler()->notify_ready(actors[3], 2, INT64_MAX);

        std::cout << "  PASS: notify_ready accepts different priorities"
                  << std::endl;
    }
    std::cout << "  ActorSystem destroyed cleanly" << std::endl;

    // Test 3: notify_ready with deadlines
    std::cout << "Test: notify_ready with deadlines (EDF)..." << std::endl;
    {
        Config config{.scheduler_threads = 2, .max_queue_depth = 1024};
        ActorSystem system(config);

        ActorId actors[] = {ActorId{101}, ActorId{102}, ActorId{103}};
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();

        system.scheduler()->notify_ready(actors[0], 2, now + 10'000'000);
        system.scheduler()->notify_ready(actors[1], 2, now + 1'000'000);
        system.scheduler()->notify_ready(actors[2], 2, now + 5'000'000);

        std::cout << "  PASS: notify_ready accepts deadlines" << std::endl;
    }
    std::cout << "  ActorSystem destroyed cleanly" << std::endl;

    // Test 4: is_running
    std::cout << "Test: is_running..." << std::endl;
    {
        Config config{.scheduler_threads = 1, .max_queue_depth = 1024};
        ActorSystem system(config);
        assert(system.is_running() == true);
        std::cout << "  PASS: is_running returns true" << std::endl;
    }
    std::cout << "  ActorSystem destroyed cleanly" << std::endl;

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
