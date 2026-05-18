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
#include <cstdint>
#include <thread>

using namespace hpactor;

// Test actor that records arrival of a scheduled self-message.
class ScheduleTestActor : public EventBasedActor {
  public:
    ScheduleTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        start_time_ = std::chrono::steady_clock::now().time_since_epoch().count();
        become(make_behavior());
    }

    bool received() const {
        return received_.load(std::memory_order_acquire);
    }
    int64_t elapsed_ms() const {
        return elapsed_ms_;
    }

    AlarmHandle trigger_schedule(std::chrono::milliseconds delay) {
        return context()->schedule(delay,
                                   TypedMessage(TypeTag::User, StreamBuffer{42}));
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == TypeTag::User) {
                auto now =
                    std::chrono::steady_clock::now().time_since_epoch().count();
                elapsed_ms_ = (now - start_time_) / 1'000'000;
                received_.store(true, std::memory_order_release);
            }
        }};
    }

  private:
    std::atomic<bool> received_{false};
    int64_t start_time_ = 0;
    int64_t elapsed_ms_ = 0;
};

// Test 1: scheduled message is delivered after the delay.
static void test_schedule_delivers_after_delay() {
    Config config;
    config.scheduler_threads = 2; // need timer thread
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());
    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(50));
    assert(alarm.id() != 0);

    // Poll with generous timeout (5s).
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!actor->received() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    assert(actor->received());
    int64_t elapsed = actor->elapsed_ms();
    // Allow 10ms undershoot, generous overshoot for CI load.
    assert(elapsed >= 40);
    assert(elapsed < 5000);
}

// Test 2: cancelling a schedule prevents delivery.
static void test_cancel_schedule_prevents_delivery() {
    Config config;
    config.scheduler_threads = 2;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());
    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(200));
    assert(alarm.id() != 0);

    // Cancel immediately.
    actor->context()->cancel_schedule(alarm);

    // Wait longer than the scheduled delay.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    assert(!actor->received());
}

// Test 3: invalid/cancelled handles are harmless.
static void test_cancel_schedule_edge_cases() {
    Config config;
    config.scheduler_threads = 2;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    // Cancel with invalid handle (id == 0): no-op, no crash.
    actor->context()->cancel_schedule(AlarmHandle{});
    actor->context()->cancel_schedule(AlarmHandle{0});

    // Schedule and cancel — double cancel should be harmless.
    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(100));
    actor->context()->cancel_schedule(alarm);
    actor->context()->cancel_schedule(alarm); // second cancel is no-op

    // Wait and confirm message never arrived.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    assert(!actor->received());
}

int main() {
    test_schedule_delivers_after_delay();
    test_cancel_schedule_prevents_delivery();
    test_cancel_schedule_edge_cases();
    return 0;
}
