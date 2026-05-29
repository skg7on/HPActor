// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#pragma once

#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/actor_ready_gate.hpp>
#include <hpactor/sched/work_queue.hpp>

#include <cstdint>

namespace hpactor {
class ActorSystem;
class EventBasedActor;
} // namespace hpactor

namespace hpactor::sched {

enum class ActorRunDisposition : uint8_t {
    Skipped,
    SuspendedOrIdle,
    RequeueReady,
    Terminated,
};

struct ActorRunResult {
    ActorRunDisposition disposition{ActorRunDisposition::Skipped};
    uint8_t priority{0};
    int64_t deadline_ns{INT64_MAX};
};

struct ActorExecutionContext {
    uint32_t worker_id{UINT32_MAX};
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics{nullptr};
    log::Logger* logger{nullptr};
};

class BehaviorActorRunner {
  public:
    BehaviorActorRunner(ActorSystem& system, ActorReadyGate& ready_gate) noexcept;

    ActorRunResult run(EventBasedActor& actor, const WorkItem& item,
                       const ActorExecutionContext& context) noexcept;

  private:
    ActorSystem& system_;
    ActorReadyGate& ready_gate_;
};

class ActorExecutionEngine {
  public:
    ActorExecutionEngine(ActorSystem& system, ActorReadyGate& ready_gate) noexcept;

    ActorRunResult run_behavior(EventBasedActor& actor, const WorkItem& item,
                                const ActorExecutionContext& context) noexcept;

  private:
    BehaviorActorRunner behavior_runner_;
};

} // namespace hpactor::sched
