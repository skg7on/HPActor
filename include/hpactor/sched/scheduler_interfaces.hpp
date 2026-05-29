// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0

#pragma once

#include <hpactor/adt/id.hpp>
#include <hpactor/adt/tags.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <functional>

namespace hpactor::sched {

using TimerHandle = Id<TimerTag>;
using timer_callback = std::function<void()>;

class IActorReadyNotifier {
  public:
    virtual ~IActorReadyNotifier() = default;
    virtual void
    notify_ready(ActorId actor, uint8_t priority, int64_t deadline_ns) = 0;
};

class ITimerService {
  public:
    virtual ~ITimerService() = default;
    virtual TimerHandle schedule_after(timer_callback cb, int64_t delay_ns) = 0;
    virtual void cancel_timer(TimerHandle handle) = 0;
};

class IActorYieldScheduler {
  public:
    virtual ~IActorYieldScheduler() = default;
    virtual void yield(ActorId actor, uint8_t priority) = 0;
};

} // namespace hpactor::sched
