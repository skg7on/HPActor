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

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/metrics/metrics_registry.hpp>
#include <hpactor/msg/delivery_result.hpp>

#include <gtest/gtest.h>

using namespace hpactor::metrics;

// Fixture for tests that need an ActorSystem
class MetricsAggregatorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        hpactor::Config cfg;
        cfg.scheduler_threads = 0;
        system_ = std::make_unique<hpactor::ActorSystem>(cfg);
        registry_ = std::make_unique<MetricRegistry>();
        agg_ = std::make_unique<Aggregator>(*registry_, *system_);
    }

    MetricRegistry& registry() {
        return *registry_;
    }
    Aggregator& agg() {
        return *agg_;
    }

  private:
    std::unique_ptr<hpactor::ActorSystem> system_;
    std::unique_ptr<MetricRegistry> registry_;
    std::unique_ptr<Aggregator> agg_;
};

TEST_F(MetricsAggregatorTest, FamiliesRegisteredViaBeginDrain) {
    agg().begin_drain();
    agg().end_drain();
    auto snapshot = registry().snapshot();
    EXPECT_GE(snapshot.families.size(), 13u);
    agg().begin_drain();
    agg().end_drain();
    auto snapshot2 = registry().snapshot();
    EXPECT_EQ(snapshot2.families.size(), snapshot.families.size());
}

TEST_F(MetricsAggregatorTest, MailboxEnqueueDequeueEvents) {
    agg().begin_drain();
    MetricEvent evt{};
    evt.actor_id = hpactor::ActorId{1};
    evt.event_type = MetricEventType::kMailboxEnqueue;
    agg().on_event(evt);
    MetricEvent evt2{};
    evt2.actor_id = hpactor::ActorId{1};
    evt2.event_type = MetricEventType::kMailboxDequeue;
    agg().on_event(evt2);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, MessageProcessedEvent) {
    agg().begin_drain();
    MetricEvent evt{};
    evt.actor_id = hpactor::ActorId{2};
    evt.event_type = MetricEventType::kMessageProcessed;
    evt.value_hi = 1500000;
    agg().on_event(evt);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, ActorLifecycleEvents) {
    agg().begin_drain();
    MetricEvent spawned{};
    spawned.actor_id = hpactor::ActorId{3};
    spawned.event_type = MetricEventType::kActorSpawned;
    agg().on_event(spawned);
    MetricEvent terminated{};
    terminated.actor_id = hpactor::ActorId{3};
    terminated.event_type = MetricEventType::kActorTerminated;
    agg().on_event(terminated);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, SchedulerEvents) {
    agg().begin_drain();
    MetricEvent dispatch{};
    dispatch.event_type = MetricEventType::kSchedulerDispatch;
    dispatch.value_hi = 0;
    agg().on_event(dispatch);
    MetricEvent steal{};
    steal.event_type = MetricEventType::kSchedulerSteal;
    steal.value_hi = 1;
    agg().on_event(steal);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, MemoryEvents) {
    agg().begin_drain();
    MetricEvent alloc{};
    alloc.actor_id = hpactor::ActorId{4};
    alloc.event_type = MetricEventType::kMemoryAlloc;
    alloc.value_hi = 1024;
    agg().on_event(alloc);
    MetricEvent free_evt{};
    free_evt.actor_id = hpactor::ActorId{4};
    free_evt.event_type = MetricEventType::kMemoryFree;
    free_evt.value_hi = 512;
    agg().on_event(free_evt);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, MailboxRejectionEvents) {
    agg().begin_drain();
    MetricEvent rejected{};
    rejected.actor_id = hpactor::ActorId{5};
    rejected.event_type = MetricEventType::kMailboxRejected;
    agg().on_event(rejected);
    MetricEvent dropped{};
    dropped.actor_id = hpactor::ActorId{5};
    dropped.event_type = MetricEventType::kMailboxDropped;
    agg().on_event(dropped);
    MetricEvent dl{};
    dl.actor_id = hpactor::ActorId{5};
    dl.event_type = MetricEventType::kMailboxDeadLetter;
    agg().on_event(dl);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, BackpressureAndDlLost) {
    agg().begin_drain();
    MetricEvent bp{};
    bp.actor_id = hpactor::ActorId{6};
    bp.event_type = MetricEventType::kBackpressureSignal;
    agg().on_event(bp);
    MetricEvent lost{};
    lost.actor_id = hpactor::ActorId{6};
    lost.event_type = MetricEventType::kDeadLetterLost;
    agg().on_event(lost);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, SupervisorRestartEvent) {
    agg().begin_drain();
    MetricEvent evt{};
    evt.actor_id = hpactor::ActorId{7};
    evt.event_type = MetricEventType::kSupervisorRestart;
    agg().on_event(evt);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, StubEventsNoop) {
    agg().begin_drain();
    MetricEvent e{};
    e.actor_id = hpactor::ActorId{8};
    e.event_type = MetricEventType::kLifecycleTransition;
    agg().on_event(e);
    e.event_type = MetricEventType::kMessageRejected;
    agg().on_event(e);
    e.event_type = MetricEventType::kActorDrainStart;
    agg().on_event(e);
    e.event_type = MetricEventType::kActorDrainComplete;
    agg().on_event(e);
    e.event_type = MetricEventType::kActorDrainTimeout;
    agg().on_event(e);
    agg().end_drain();
}

TEST_F(MetricsAggregatorTest, EndDrainRecordsActive) {
    agg().begin_drain();
    agg().end_drain();
    auto snapshot = registry().snapshot();
    bool found = false;
    for (auto& fam : snapshot.families) {
        if (fam.name.find("hpactor_actors_active") != std::string::npos) {
            found = true;
            break;
        }
    }
    EXPECT_TRUE(found);
}

TEST_F(MetricsAggregatorTest, DeliveryResultEvents) {
    agg().begin_drain();

    // Accepted
    MetricEvent accepted{};
    accepted.actor_id = hpactor::ActorId{10};
    accepted.event_type = MetricEventType::kDeliveryResult;
    accepted.code =
        static_cast<uint8_t>(hpactor::mailbox::DeliveryStatus::Accepted);
    accepted.value_hi = 1;
    agg().on_event(accepted);

    // MailboxFull
    MetricEvent rejected{};
    rejected.actor_id = hpactor::ActorId{10};
    rejected.event_type = MetricEventType::kDeliveryResult;
    rejected.code =
        static_cast<uint8_t>(hpactor::mailbox::DeliveryStatus::MailboxFull);
    rejected.value_hi = 1;
    agg().on_event(rejected);

    // NoRoute
    MetricEvent no_route{};
    no_route.actor_id = hpactor::ActorId{10};
    no_route.event_type = MetricEventType::kDeliveryResult;
    no_route.code = static_cast<uint8_t>(hpactor::mailbox::DeliveryStatus::NoRoute);
    no_route.value_hi = 1;
    agg().on_event(no_route);

    agg().end_drain();

    auto snapshot = registry().snapshot();
    const MetricRegistry::Snapshot::FamilySnapshot* family = nullptr;
    for (auto& fam : snapshot.families) {
        if (fam.name == "hpactor_delivery_results_total") {
            family = &fam;
            break;
        }
    }
    ASSERT_NE(family, nullptr) << "hpactor_delivery_results_total family not "
                                  "found";

    // Count by status label
    uint64_t total = 0;
    for (auto& [labels, value] : family->counters) {
        total += value;
    }
    EXPECT_EQ(total, 3u);

    // Verify individual status labels exist
    bool has_accepted = false;
    bool has_mailbox_full = false;
    bool has_no_route = false;
    for (auto& [labels, value] : family->counters) {
        for (auto& [k, v] : labels.labels) {
            if (k == "status") {
                if (v == "accepted")
                    has_accepted = true;
                if (v == "mailbox_full")
                    has_mailbox_full = true;
                if (v == "no_route")
                    has_no_route = true;
            }
        }
    }
    EXPECT_TRUE(has_accepted);
    EXPECT_TRUE(has_mailbox_full);
    EXPECT_TRUE(has_no_route);
}
