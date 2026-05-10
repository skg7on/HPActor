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
#include <chrono>
#include <climits>
#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/actor/dense_computing_actor.hpp>
#include <hpactor/actor/polling_actor.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>
#include <thread>

using namespace hpactor;

class PoolProbeActor : public DenseComputingActor {
  public:
    PoolProbeActor(ActorContext* ctx, ActorSystem& sys)
        : DenseComputingActor(ctx, sys, /*pool_size=*/2) {}

    std::atomic<bool> handled{false};
    std::atomic<uint32_t> worker_id{0};

    void on_activate() override {
        become(Behavior{[this](TypedMessage&) {
            auto* scheduler =
                static_cast<sched::HybridScheduler*>(system().scheduler());
            worker_id.store(scheduler->current_worker_id(),
                            std::memory_order_release);
            handled.store(true, std::memory_order_release);
        }});
        EventBasedActor::on_activate();
    }
};

static void wait_for_probe(PoolProbeActor* actor) {
    for (int i = 0; i < 50 && !actor->handled.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Verify DispatchPolicy values propagate correctly through the hierarchy
int main() {
    Config config;
    config.enable_network = false;
    config.scheduler_threads = 2;

    ActorSystem system(config);

    // Test 1: Default EventBasedActor is Cooperative
    {
        auto actor = system.spawn<EventBasedActor>();
        auto* raw = static_cast<EventBasedActor*>(actor.get().get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::Cooperative);
    }

    // Test 2: DaemonActor is DedicatedThread
    {
        class SimpleDaemon : public DaemonActor {
          public:
            SimpleDaemon(ActorContext* ctx, ActorSystem& sys)
                : DaemonActor(ctx, sys) {}
            std::atomic<bool> ran{false};
            bool run_once() override {
                ran.store(true);
                return false; // one-shot
            }
        };

        auto actor = system.spawn<SimpleDaemon>();
        auto* raw = static_cast<SimpleDaemon*>(actor.get().get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::DedicatedThread);

        // Wait for daemon to run
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        assert(raw->ran.load());
    }

    // Test 3: PollingActor is DedicatedThread
    {
        class SimplePoller : public PollingActor {
          public:
            SimplePoller(ActorContext* ctx, ActorSystem& sys)
                : PollingActor(ctx, sys, /*cpu_core=*/-1) {}
            bool run_once() override {
                return false; // one-shot
            }
        };

        auto actor = system.spawn<SimplePoller>();
        auto* raw = static_cast<SimplePoller*>(actor.get().get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::DedicatedThread);
    }

    // Test 4: DenseComputingActor is DedicatedPool
    {
        auto actor = system.spawn<DenseComputingActor>(/*pool_size=*/4);
        auto* raw = static_cast<DenseComputingActor*>(actor.get().get());
        assert(raw != nullptr);
        assert(raw->dispatch_policy() == sched::DispatchPolicy::DedicatedPool);

        auto hints = raw->dispatch_hints();
        assert(hints.pool_size == 4);
    }

    // Test 5: DedicatedPool actors are dispatched outside cooperative workers
    {
        auto actor = system.spawn<PoolProbeActor>();
        auto* raw = static_cast<PoolProbeActor*>(actor.get().get());
        assert(raw != nullptr);

        system.deliver_local(actor.id(),
                             TypedMessage(TypeTag::User, StreamBuffer{}));
        wait_for_probe(raw);

        assert(raw->handled.load(std::memory_order_acquire));
        assert(raw->worker_id.load(std::memory_order_acquire) == UINT32_MAX);
    }

    // Test 6: spawn_configured preserves the actor's own dispatch policy
    {
        config::ActorDef def;
        def.behavior = "PoolProbeActor";

        auto actor = system.spawn_configured(
            std::make_shared<PoolProbeActor>(nullptr, system), def);
        auto* raw = static_cast<PoolProbeActor*>(actor.get().get());
        assert(raw != nullptr);

        system.deliver_local(actor.id(),
                             TypedMessage(TypeTag::User, StreamBuffer{}));
        wait_for_probe(raw);

        assert(raw->handled.load(std::memory_order_acquire));
        assert(raw->worker_id.load(std::memory_order_acquire) == UINT32_MAX);
    }

    // Test 7: DispatchHints default values
    {
        sched::DispatchHints hints;
        assert(hints.cpu_affinity == -1);
        assert(hints.pool_size == 1);
        assert(hints.priority == 0);
    }

    return 0;
}
