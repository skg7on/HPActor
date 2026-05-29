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
