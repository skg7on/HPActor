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
// HPActor Example 08: Coroutine Scheduling and Context Switching
// =============================================================================
//
// This example demonstrates coroutine-based actor scheduling:
//   - Multiple actors running concurrently on a HybridScheduler
//   - Actor state machine: Idle, Ready, Running, IOWaiting, Terminated
//   - Context switching via C++20 coroutines when actors wait for messages
//   - Edge-trigger mailbox wakeup mechanism
//   - Work-stealing load balancing across scheduler threads
//
// Run: ./build/examples/08_coroutine_scheduler_demo
//
// Note: Message types (ping_msg, pong_msg, stop_msg, start_msg) are defined
// in the framework's abstract_actor.hpp for convenience. In a real application,
// custom message types would be defined in user code and included via a
// user-defined MessageVariant type.
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <hpactor/sched/coroutine_awaiters.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/actor/message.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>

using namespace hpactor;

// -----------------------------------------------------------------------------
// Example message types (ping_msg, pong_msg, etc.) are defined in the
// framework's abstract_actor.hpp as part of MessageVariant for convenience.
// In a real application, you would define your own message types and
// compose them with system messages in your own ApplicationMessageVariant.
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// Shared state for demonstration output
// -----------------------------------------------------------------------------
static std::atomic<int> g_context_switches{0};
static std::atomic<bool> g_done{false};

// -----------------------------------------------------------------------------
// EchoActor - receives messages and prints what it got
// -----------------------------------------------------------------------------
class EchoActor : public hpactor::EventBasedActor {
  public:
    EchoActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}

    hpactor::sched::CoroutineTask act() override {
        std::cout << "[EchoActor] Started on thread " << std::this_thread::get_id() << "\n";

        int count = 0;
        while (count < 3 && !g_done.load()) {
            auto msg = co_await make_mailbox_awaiter();
            auto& variant = msg.payload();

            if (std::holds_alternative<ping_msg>(variant)) {
                auto& ping = std::get<ping_msg>(variant);
                std::cout << "[EchoActor] Got ping #" << ping.sequence
                          << " from node " << hpactor::endpoint_ops::to_string(ping.from.endpoint)
                          << " -> thread " << std::this_thread::get_id() << "\n";
                ++count;
                ++g_context_switches;
            }
        }

        std::cout << "[EchoActor] Finished after " << count << " messages\n";
        co_return;
    }
};

// -----------------------------------------------------------------------------
// Demo: Single actor spawn
// -----------------------------------------------------------------------------
void demo_single_actor() {
    std::cout << "\n=== Demo 1: Single Actor Spawn ===" << std::endl;

    hpactor::Config config{
        .scheduler_threads = 4,
        .max_queue_depth = 1024
    };
    hpactor::ActorSystem system(config);

    std::cout << "Scheduler state:\n";
    std::cout << "  Worker threads: " << system.scheduler()->worker_count() << "\n";
    std::cout << "  Running: " << (system.scheduler()->is_running() ? "yes" : "no") << "\n";

    // Spawn an echo actor
    auto echo_ref = system.spawn<EchoActor>();
    std::cout << "Spawned EchoActor at node " << hpactor::endpoint_ops::to_string(echo_ref.address().endpoint) << "\n";

    // Send it a message
    system.deliver_local(echo_ref.address().id, ping_msg{echo_ref.address(), 0});

    // Let scheduler run for a moment
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Signal done and let actors finish
    g_done = true;

    // System destruction will properly stop scheduler
    std::cout << "Single actor demo complete\n";
}

// -----------------------------------------------------------------------------
// Demo: Multiple actors with message exchange
// -----------------------------------------------------------------------------
void demo_multi_actor() {
    std::cout << "\n=== Demo 2: Multiple Actors with Message Exchange ===" << std::endl;

    g_context_switches = 0;
    g_done = false;

    hpactor::Config config{
        .scheduler_threads = 4,
        .max_queue_depth = 1024
    };
    hpactor::ActorSystem system(config);

    // Spawn actors
    auto echo1_ref = system.spawn<EchoActor>();
    auto echo2_ref = system.spawn<EchoActor>();

    std::cout << "Spawned EchoActor1 at node " << hpactor::endpoint_ops::to_string(echo1_ref.address().endpoint) << "\n";
    std::cout << "Spawned EchoActor2 at node " << hpactor::endpoint_ops::to_string(echo2_ref.address().endpoint) << "\n";

    // Send messages to both echo actors
    system.deliver_local(echo1_ref.address().id,
        ping_msg{echo1_ref.address(), 1});
    system.deliver_local(echo2_ref.address().id,
        ping_msg{echo2_ref.address(), 2});
    system.deliver_local(echo1_ref.address().id,
        ping_msg{echo1_ref.address(), 3});

    // Let the actors exchange messages
    std::cout << "\nLetting actors exchange messages...\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Signal done
    g_done = true;

    std::cout << "\nResults:\n";
    std::cout << "  Context switches: " << g_context_switches.load() << "\n";
}

// -----------------------------------------------------------------------------
// Demo: Scheduler architecture explanation
// -----------------------------------------------------------------------------
void demo_scheduler_architecture() {
    std::cout << "\n=== Demo 3: HybridScheduler Architecture ===" << std::endl;

    std::cout << R"(
HybridScheduler Components:

1. Worker Threads (config.scheduler_threads = 4)
   - Each worker has its own ChaseLev deque (priority queues)
   - Work-stealing for load balancing

2. Priority Queues (4 levels: 0=highest, 3=lowest)
   - Per-worker, per-priority queues
   - Local operations are wait-free (push_bottom, pop_bottom)

3. EDF Queue (Earliest Deadline First)
   - For time-sensitive work with deadlines
   - Checked after local priority queues

4. A2WS (Adaptive Two-Level Work Stealing)
   - Victim selection algorithm
   - Records steal history for better decisions

5. Timing Wheel
   - For timer management
   - Schedule one-shot and recurring timers

Key APIs:
  notify_ready(actor, priority, deadline) — Add actor to scheduler
  yield(actor, priority)                — Voluntary yield
  schedule_after(delay, callback)        — One-shot timer
  schedule_every(interval, callback)   — Recurring timer
)";
}

// -----------------------------------------------------------------------------
// Demo: State transitions explanation
// -----------------------------------------------------------------------------
void demo_state_machine() {
    std::cout << "\n=== Demo 4: Actor State Machine ===" << std::endl;

    std::cout << R"(
Actor State Transitions:

  +---------+       spawn        +---------+
  |  Idle   | ----------------> |  Ready  |
  +---------+                  +---------+
       ^                              |
       |                              | execute_actor() picks up
       |                              | CAS(Ready → Running)
       |                              v
       |                      +---------+
       |                      | Running |
       |                      +---------+
       |                         |
       |    +--------------------+--------------------+--+
       |    |                    |                    |  |
       |    v                    v                    v  v
       | co_await           co_await             co_return
       | (mailbox empty)   yield (re-enqueue)
       |    |                    |
       |    |                    |  (Ready → Running)
       |    v                    v
       | +---------+      +---------+
       | | Idle    |<---- | Ready   |
       | +---------+      +---------+
       |    ^                   |
       |    | (message          |
       |    |  arrives)          |
       |    +--------------------+
       |
       |    +-----------+
       |    | IOWaiting |
       +--->+-----------+
       |    | (timer/I/O)
       |    +-----------+
       |         |
       |         | timer fires / I/O completes
       |         v
       +--- +---------+
            |  Ready  |
            +---------+

Key Points:
  - All transitions use atomic CAS (no locks in hot path)
  - CAS failure = transition not valid (another thread handled it)
  - Edge-trigger: Idle→Ready only on FIRST message after empty
  - Running→Idle stores continuation for later resumption
)";
}

// -----------------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------------
int main() {
    std::cout << "================================================================" << std::endl;
    std::cout << " HPActor Example 08: Coroutine Scheduling and Context Switching" << std::endl;
    std::cout << "================================================================" << std::endl;

    // Run demos
    demo_scheduler_architecture();
    demo_state_machine();
    demo_single_actor();
    demo_multi_actor();

    std::cout << "\n================================================================" << std::endl;
    std::cout << " Example 08 Complete" << std::endl;
    std::cout << "================================================================" << std::endl;

    return 0;
}
