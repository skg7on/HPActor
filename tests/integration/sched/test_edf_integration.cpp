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
// Test: EDF Queue Integration — end-to-end EDF dispatch ordering.
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/sched/scheduler.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

namespace {

class NoopActor : public EventBasedActor {
  public:
    using EventBasedActor::EventBasedActor;
};

} // namespace

// ── notify_ready_edf accepts spawned actors ──────────────────────────────────

TEST(EdfIntegrationTest, NotifyReadyEdfSmoke) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    cfg.enable_receptionist = false;
    ActorSystem system(cfg);

    auto a = system.spawn<NoopActor>();

    auto* sched = static_cast<sched::HybridScheduler*>(system.scheduler());
    // Drain initial spawn notification.
    sched->drain_ready(1);

    // notify_ready_edf should succeed (actor is now idle).
    sched->notify_ready_edf(a.id(), 0, 5'000'000);
    SUCCEED();

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system.shutdown(opts);
}

// ── edf_next_deadline reflects populated EDF queue ───────────────────────────

TEST(EdfIntegrationTest, EdfNextDeadlineReflectsEdfQueue) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    cfg.enable_receptionist = false;
    ActorSystem system(cfg);

    auto a = system.spawn<NoopActor>();
    auto* sched = static_cast<sched::HybridScheduler*>(system.scheduler());
    // Drain initial spawn notification.
    sched->drain_ready(1);

    // Before any EDF work, next deadline should be INT64_MAX.
    EXPECT_EQ(sched->edf_next_deadline(), INT64_MAX);

    // Enqueue via EDF path.
    sched->notify_ready_edf(a.id(), 0, 5'000'000);

    // Now edf_next_deadline should return the deadline.
    int64_t deadline = sched->edf_next_deadline();
    EXPECT_NE(deadline, INT64_MAX);

    // Drain the EDF item.
    auto drained = sched->drain_ready(1);
    EXPECT_EQ(drained.executed, 1u);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system.shutdown(opts);
}

// ── EDF items are drainable after priority items ─────────────────────────────

TEST(EdfIntegrationTest, EdfAndPriorityItemsBothDrainable) {
    Config cfg;
    cfg.scheduler_threads = 1;
    cfg.scheduler_start_paused = true;
    cfg.enable_network = false;
    cfg.enable_receptionist = false;
    ActorSystem system(cfg);

    auto a = system.spawn<NoopActor>();
    auto b = system.spawn<NoopActor>();
    auto* sched = static_cast<sched::HybridScheduler*>(system.scheduler());
    // Drain initial spawn notifications.
    auto initial = sched->drain_ready(2);
    ASSERT_EQ(initial.executed, 2u);

    // Enqueue one priority item and one EDF item.
    sched->notify_ready(a.id(), 0, INT64_MAX);
    sched->notify_ready_edf(b.id(), 0, 1'000'000);

    // Both should be drainable.
    auto drained = sched->drain_ready(2);
    EXPECT_EQ(drained.executed, 2u);

    ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(10);
    opts.actor_drain_timeout = std::chrono::milliseconds(10);
    opts.cluster_leave_timeout = std::chrono::milliseconds(10);
    system.shutdown(opts);
}
