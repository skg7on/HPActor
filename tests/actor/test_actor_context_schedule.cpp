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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>

#include <atomic>
#include <cassert>
#include <chrono>
#include <thread>

using namespace hpactor;

// Test actor that records arrival of a scheduled self-message.
class ScheduleTestActor : public EventBasedActor {
  public:
    ScheduleTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    bool received() const {
        return received_.load(std::memory_order_acquire);
    }

    AlarmHandle trigger_schedule(std::chrono::milliseconds delay) {
        return context()->schedule(delay,
                                   TypedMessage(TypeTag::User, StreamBuffer{42}));
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == TypeTag::User) {
                received_.store(true, std::memory_order_release);
            }
        }};
    }

  private:
    std::atomic<bool> received_{false};
};

// Test 1: schedule() returns valid handle when scheduler is available.
static void test_schedule_returns_valid_handle() {
    Config config;
    config.scheduler_threads = 1;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());
    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(5000));
    assert(alarm.id() != 0);
    actor->context()->cancel_schedule(alarm);
}

// Test 2: cancel on invalid handles is always safe.
static void test_cancel_invalid_handles() {
    Config config;
    config.scheduler_threads = 1;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    actor->context()->cancel_schedule(AlarmHandle{});
    actor->context()->cancel_schedule(AlarmHandle{0});
}

// Test 3: cancelling a schedule before it fires prevents delivery.
static void test_cancel_schedule_prevents_delivery() {
    Config config;
    config.scheduler_threads = 1;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(5000));
    assert(alarm.id() != 0);

    actor->context()->cancel_schedule(alarm);
    actor->context()->cancel_schedule(alarm); // double cancel is harmless

    // Brief wait — timer is 5s out, shouldn't fire in 100ms.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    assert(!actor->received());
}

int main() {
    test_schedule_returns_valid_handle();
    test_cancel_invalid_handles();
    test_cancel_schedule_prevents_delivery();
    return 0;
}
