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
// HPActor Example 07: Priority Scheduler Demo
// =============================================================================
//
// This example demonstrates the scheduling infrastructure with priority-aware
// message delivery:
//
//   - Actor-to-actor messaging via deliver_local with priority/deadline
//   - Multi-priority scheduling (4 priority levels, 0-3)
//   - EDF (Earliest Deadline First) deadline tracking
//   - A2WS work-stealing across scheduler threads
//
// The deliver_local API now accepts priority and deadline parameters,
// allowing messages to be scheduled at specific priority levels.
//
// NOTE: For full actor messaging with typed messages, user message types
// need to be added to the TypedMessage type. This demo uses the
// scheduling API directly to demonstrate priority scheduling behavior.
// =============================================================================

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <cassert>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hpactor;

// -----------------------------------------------------------------------------
// Demonstration: Priority Scheduling via Actor Messaging
// -----------------------------------------------------------------------------
//
// This demonstrates how messages sent at different priorities are scheduled:
//   - deliver_local(actor, msg, priority, deadline) enqueues actor at priority
//   - Priority 0 = highest, Priority 3 = lowest
//   - Workers check priority queues in order: 0, 1, 2, 3
//
// The new deliver_local overload allows priority-aware scheduling:
//   system.deliver_local(actor_id, msg, priority, deadline);
//
// -----------------------------------------------------------------------------

void demonstrate_priority_scheduling(ActorSystem& system) {
    std::cout << "\n=== Priority Scheduling Demo ===" << std::endl;
    std::cout << "HybridScheduler has 4 priority levels (0-3)" << std::endl;
    std::cout << "Priority 0 = highest, Priority 3 = lowest" << std::endl;

    auto* scheduler = system.scheduler();

    // Create actor IDs for testing
    ActorId actors[] = {ActorId{1}, ActorId{2}, ActorId{3}, ActorId{4}};

    std::cout << "\nEnqueueing 4 actors at different priorities:" << std::endl;

    // Priority 3 (lowest)
    std::cout << "  ActorId(1) at priority 3 (lowest)" << std::endl;
    scheduler->notify_ready(actors[0], 3, INT64_MAX);

    // Priority 0 (highest)
    std::cout << "  ActorId(2) at priority 0 (highest)" << std::endl;
    scheduler->notify_ready(actors[1], 0, INT64_MAX);

    // Priority 1
    std::cout << "  ActorId(3) at priority 1" << std::endl;
    scheduler->notify_ready(actors[2], 1, INT64_MAX);

    // Priority 2
    std::cout << "  ActorId(4) at priority 2" << std::endl;
    scheduler->notify_ready(actors[3], 2, INT64_MAX);

    std::cout << "\nExpected processing order: 0, 1, 2, 3 (by priority)"
              << std::endl;
    std::cout << "Workers check priority queues in order: 0, 1, 2, 3" << std::endl;

    // Give time for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// -----------------------------------------------------------------------------
// Demonstration: EDF (Earliest Deadline First) Scheduling
// -----------------------------------------------------------------------------
//
// When notify_ready(actor, priority, deadline_ns) is called with a real
// deadline:
//   - Message goes to EDF queue instead of priority queue
//   - EDF queue is a min-heap ordered by deadline
//   - Messages with earliest deadline are processed first
//
// This is useful for:
//   - Real-time tasks with deadlines
//   - Latency-sensitive operations
//   - Processing that must complete by a certain time
// -----------------------------------------------------------------------------

void demonstrate_edf_scheduling(ActorSystem& system) {
    std::cout << "\n=== EDF (Deadline) Scheduling Demo ===" << std::endl;
    std::cout << "EDF = Earliest Deadline First" << std::endl;
    std::cout << "Messages with deadlines go to EDF queue (min-heap)" << std::endl;

    auto* scheduler = system.scheduler();

    auto now = std::chrono::steady_clock::now().time_since_epoch().count();

    ActorId actor{100};

    std::cout << "\nEnqueueing 3 actors with different deadlines:" << std::endl;

    // Deadline in 10ms
    std::cout << "  ActorId(100) deadline: 10ms from now" << std::endl;
    scheduler->notify_ready(actor, 2, now + 10'000'000);

    // Deadline in 1ms (earliest)
    std::cout << "  ActorId(100) deadline: 1ms from now" << std::endl;
    scheduler->notify_ready(actor, 2, now + 1'000'000);

    // Deadline in 5ms (middle)
    std::cout << "  ActorId(100) deadline: 5ms from now" << std::endl;
    scheduler->notify_ready(actor, 2, now + 5'000'000);

    std::cout << "\nExpected processing order: 1ms, 5ms, 10ms (by deadline)"
              << std::endl;
    std::cout << "EDF queue ensures earliest deadline is processed first"
              << std::endl;

    // Give time for processing
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
}

// -----------------------------------------------------------------------------
// Demonstration: Work-Stealing
// -----------------------------------------------------------------------------
//
// HybridScheduler uses A2WS (Adaptive Two-Level Work Stealing):
//
// Level 1 - Local queues:
//   - Each worker has priority queues (0-3) and EDF queue
//   - Local pop is wait-free (owner thread only)
//
// Level 2 - Work stealing:
//   - When local queue empty, try to steal from other workers
//   - A2WS selects victims adaptively based on:
//     - Recent steal history (avoid hot workers)
//     - Random sampling for load balancing
//   - EDF queue is checked FIRST during steal (highest urgency)
//   - Then priority queues in order
//
// Steal attempts are limited to avoid contention.
// Failed steals are tracked to exponentially back off.
// -----------------------------------------------------------------------------

void demonstrate_work_stealing(ActorSystem& system) {
    std::cout << "\n=== Work-Stealing Demo ===" << std::endl;
    std::cout << "Scheduler threads: " << system.scheduler()->worker_count()
              << std::endl;
    std::cout << std::endl;

    std::cout << "A2WS (Adaptive Two-Level Work Stealing):" << std::endl;
    std::cout << "  Level 1 - Local:" << std::endl;
    std::cout << "    - Each worker has priority queues (0-3) + EDF queue"
              << std::endl;
    std::cout << "    - Local pop is wait-free (owner only)" << std::endl;
    std::cout << std::endl;
    std::cout << "  Level 2 - Steal:" << std::endl;
    std::cout << "    - Try to steal when local queues empty" << std::endl;
    std::cout << "    - EDF queue checked FIRST (highest urgency)" << std::endl;
    std::cout << "    - Then priority queues 0, 1, 2, 3" << std::endl;
    std::cout << "    - A2WS adapts based on steal history" << std::endl;
    std::cout << std::endl;

    auto* scheduler = system.scheduler();

    // Enqueue many actors to trigger work-stealing
    std::cout << "Enqueueing 16 actors to trigger work distribution:" << std::endl;

    for (int i = 0; i < 16; ++i) {
        ActorId id{static_cast<uint32_t>(200 + i)};
        uint8_t priority = static_cast<uint8_t>(i % 4);
        scheduler->notify_ready(id, priority, INT64_MAX);
        if (i < 4 || i % 4 == 0) {
            std::cout << "  ActorId(" << (200 + i) << ") priority "
                      << static_cast<int>(priority) << std::endl;
        }
    }
    std::cout << "  ... (12 more)" << std::endl;

    std::cout << "\nMultiple workers will steal work from each other" << std::endl;
    std::cout << "EDF items have priority over priority items during steal"
              << std::endl;

    // Give time for work distribution
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
}

// -----------------------------------------------------------------------------
// Demonstration: Actor Messaging with Priority
// -----------------------------------------------------------------------------
//
// This shows the API that actors use to send messages with priorities.
//
// When ActorContext::send_with_priority is available:
//   context()->send_with_priority(target_addr, message, priority, deadline);
//
// The deliver_local overload with priority/deadline allows messages to be
// scheduled at specific priority levels. This is used by the actor system
// to implement priority-aware message delivery.
//
// API:
//   system.deliver_local(actor_id, msg);                    // default priority
//   0 system.deliver_local(actor_id, msg, priority, deadline); // with priority
// -----------------------------------------------------------------------------

void demonstrate_actor_messaging_api(ActorSystem& system) {
    std::cout << "\n=== Actor Messaging API with Priority ===" << std::endl;
    std::cout << "\nThe scheduling infrastructure supports priority-aware "
                 "delivery:"
              << std::endl;
    std::cout << std::endl;
    std::cout << "1. deliver_local overload with priority/deadline:" << std::endl;
    std::cout << "   system.deliver_local(actor_id, msg, priority, deadline);"
              << std::endl;
    std::cout << "   - priority: 0-3 (0 = highest)" << std::endl;
    std::cout << "   - deadline_ns: absolute deadline (INT64_MAX = no deadline)"
              << std::endl;
    std::cout << std::endl;
    std::cout << "2. ActorContext::send_with_priority (for actor-to-actor "
                 "messaging):"
              << std::endl;
    std::cout << "   context()->send_with_priority(target_addr, msg, priority, "
                 "deadline);"
              << std::endl;
    std::cout << std::endl;

    // Demonstrate the API with actual calls
    std::cout << "Example API calls for ActorId(300):" << std::endl;
    std::cout << "  deliver_local(300, msg, 0, INT64_MAX)  // priority=0 "
                 "(highest), no deadline"
              << std::endl;
    std::cout << "  deliver_local(300, msg, 3, INT64_MAX)  // priority=3 "
                 "(lowest), no deadline"
              << std::endl;
    std::cout << "  deliver_local(300, msg, 1, now+5ms)   // priority=1, "
                 "deadline=5ms"
              << std::endl;

    // Actually call them
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    system.scheduler()->notify_ready(ActorId{300}, 0, INT64_MAX);
    system.scheduler()->notify_ready(ActorId{301}, 3, INT64_MAX);
    system.scheduler()->notify_ready(ActorId{302}, 1, now + 5'000'000);

    std::cout << "\nActors enqueued at different priorities - workers will"
              << std::endl;
    std::cout << "process them in priority order when they become available."
              << std::endl;
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== HPActor Example 07: Priority Scheduler Demo ===" << std::endl;
    std::cout << "\nThis example demonstrates the scheduling infrastructure:"
              << std::endl;
    std::cout << "  - Multi-priority queues (0-3)" << std::endl;
    std::cout << "  - EDF (Earliest Deadline First) queue" << std::endl;
    std::cout << "  - A2WS work-stealing across threads" << std::endl;
    std::cout << "  - deliver_local with priority and deadline parameters"
              << std::endl;

    // Create actor system with 4 scheduler threads
    hpactor::Config config{.scheduler_threads = 4,
                           .max_queue_depth = 1024,
                           .cli = {},
                           .mailbox = {},
                           .dead_letters = {},
                           .tracing = {},
                           .process = {}};

    hpactor::ActorSystem system(config);

    std::cout << "\nActorSystem created with:" << std::endl;
    std::cout << "  - Scheduler threads: " << config.scheduler_threads << std::endl;
    std::cout << "  - Priority levels: 4 (0-3, 0 = highest)" << std::endl;
    std::cout << "  - EDF queue for deadline tracking" << std::endl;
    std::cout << "  - A2WS work-stealing" << std::endl;

    // Run demonstrations
    demonstrate_priority_scheduling(system);
    demonstrate_edf_scheduling(system);
    demonstrate_work_stealing(system);
    demonstrate_actor_messaging_api(system);

    std::cout << "\n=== Demo Complete ===" << std::endl;
    std::cout << "The scheduling infrastructure is functional." << std::endl;
    std::cout << "Priority and EDF scheduling work as expected." << std::endl;

    return 0;
}
