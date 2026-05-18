#include <cassert>
#include <cstdio>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/metrics/metrics_aggregator.hpp>
#include <hpactor/metrics/metrics_registry.hpp>

using namespace hpactor::metrics;

void test_families_registered_via_begin_drain() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    agg.end_drain();
    auto snapshot = registry.snapshot();
    assert(snapshot.families.size() >= 13);
    agg.begin_drain();
    agg.end_drain();
    auto snapshot2 = registry.snapshot();
    assert(snapshot2.families.size() == snapshot.families.size());
    printf("  PASSED test_families_registered_via_begin_drain\n");
}

void test_mailbox_enqueue_dequeue_events() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent evt{};
    evt.actor_id = hpactor::ActorId{1};
    evt.event_type = MetricEventType::kMailboxEnqueue;
    agg.on_event(evt);
    MetricEvent evt2{};
    evt2.actor_id = hpactor::ActorId{1};
    evt2.event_type = MetricEventType::kMailboxDequeue;
    agg.on_event(evt2);
    agg.end_drain();
    printf("  PASSED test_mailbox_enqueue_dequeue_events\n");
}

void test_message_processed_event() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent evt{};
    evt.actor_id = hpactor::ActorId{2};
    evt.event_type = MetricEventType::kMessageProcessed;
    evt.value_hi = 1500000;
    agg.on_event(evt);
    agg.end_drain();
    printf("  PASSED test_message_processed_event\n");
}

void test_actor_lifecycle_events() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent spawned{};
    spawned.actor_id = hpactor::ActorId{3};
    spawned.event_type = MetricEventType::kActorSpawned;
    agg.on_event(spawned);
    MetricEvent terminated{};
    terminated.actor_id = hpactor::ActorId{3};
    terminated.event_type = MetricEventType::kActorTerminated;
    agg.on_event(terminated);
    agg.end_drain();
    printf("  PASSED test_actor_lifecycle_events\n");
}

void test_scheduler_events() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent dispatch{};
    dispatch.event_type = MetricEventType::kSchedulerDispatch;
    dispatch.value_hi = 0;
    agg.on_event(dispatch);
    MetricEvent steal{};
    steal.event_type = MetricEventType::kSchedulerSteal;
    steal.value_hi = 1;
    agg.on_event(steal);
    agg.end_drain();
    printf("  PASSED test_scheduler_events\n");
}

void test_memory_events() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent alloc{};
    alloc.actor_id = hpactor::ActorId{4};
    alloc.event_type = MetricEventType::kMemoryAlloc;
    alloc.value_hi = 1024;
    agg.on_event(alloc);
    MetricEvent free_evt{};
    free_evt.actor_id = hpactor::ActorId{4};
    free_evt.event_type = MetricEventType::kMemoryFree;
    free_evt.value_hi = 512;
    agg.on_event(free_evt);
    agg.end_drain();
    printf("  PASSED test_memory_events\n");
}

void test_mailbox_rejection_events() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent rejected{};
    rejected.actor_id = hpactor::ActorId{5};
    rejected.event_type = MetricEventType::kMailboxRejected;
    agg.on_event(rejected);
    MetricEvent dropped{};
    dropped.actor_id = hpactor::ActorId{5};
    dropped.event_type = MetricEventType::kMailboxDropped;
    agg.on_event(dropped);
    MetricEvent dl{};
    dl.actor_id = hpactor::ActorId{5};
    dl.event_type = MetricEventType::kMailboxDeadLetter;
    agg.on_event(dl);
    agg.end_drain();
    printf("  PASSED test_mailbox_rejection_events\n");
}

void test_backpressure_and_dl_lost() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent bp{};
    bp.actor_id = hpactor::ActorId{6};
    bp.event_type = MetricEventType::kBackpressureSignal;
    agg.on_event(bp);
    MetricEvent lost{};
    lost.actor_id = hpactor::ActorId{6};
    lost.event_type = MetricEventType::kDeadLetterLost;
    agg.on_event(lost);
    agg.end_drain();
    printf("  PASSED test_backpressure_and_dl_lost\n");
}

void test_supervisor_restart_event() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent evt{};
    evt.actor_id = hpactor::ActorId{7};
    evt.event_type = MetricEventType::kSupervisorRestart;
    agg.on_event(evt);
    agg.end_drain();
    printf("  PASSED test_supervisor_restart_event\n");
}

void test_stub_events_noop() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    MetricEvent e{};
    e.actor_id = hpactor::ActorId{8};
    e.event_type = MetricEventType::kLifecycleTransition;
    agg.on_event(e);
    e.event_type = MetricEventType::kMessageRejected;
    agg.on_event(e);
    e.event_type = MetricEventType::kActorDrainStart;
    agg.on_event(e);
    e.event_type = MetricEventType::kActorDrainComplete;
    agg.on_event(e);
    e.event_type = MetricEventType::kActorDrainTimeout;
    agg.on_event(e);
    agg.end_drain();
    printf("  PASSED test_stub_events_noop\n");
}

void test_end_drain_records_active() {
    hpactor::Config sys_cfg;
    sys_cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(sys_cfg);
    MetricRegistry registry;
    Aggregator agg(registry, sys);
    agg.begin_drain();
    agg.end_drain();
    auto snapshot = registry.snapshot();
    bool found = false;
    for (auto& fam : snapshot.families) {
        if (fam.name.find("hpactor_actors_active") != std::string::npos) {
            found = true;
            break;
        }
    }
    assert(found);
    printf("  PASSED test_end_drain_records_active\n");
}

int main() {
    printf("MetricsAggregator tests:\n");
    test_families_registered_via_begin_drain();
    test_mailbox_enqueue_dequeue_events();
    test_message_processed_event();
    test_actor_lifecycle_events();
    test_scheduler_events();
    test_memory_events();
    test_mailbox_rejection_events();
    test_backpressure_and_dl_lost();
    test_supervisor_restart_event();
    test_stub_events_noop();
    test_end_drain_records_active();
    printf("All MetricsAggregator tests PASSED\n");
    return 0;
}
