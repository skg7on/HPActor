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

#include <cassert>
#include <chrono>
#include <thread>

using namespace hpactor;

namespace {

class ScheduleTestActor : public EventBasedActor {
  public:
    ScheduleTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {
        become(make_behavior());
    }

    AlarmHandle trigger_schedule(std::chrono::milliseconds delay) {
        return context()->schedule(delay,
                                   TypedMessage(TypeTag::User, StreamBuffer{42}));
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[](TypedMessage&) {
            // No-op — delivery is verified via mailbox inspection.
        }};
    }
};

// Test 1: schedule() returns a valid (non-zero) AlarmHandle when the
// scheduler is available.  scheduler_threads=0 keeps the test
// deterministic — no worker threads, timer thread runs but we cancel
// the long-delay timer immediately.
static void test_schedule_returns_valid_handle() {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(5000));
    assert(alarm.id() != 0);

    actor->context()->cancel_schedule(alarm);
}

// Test 2: cancel_schedule() with invalid handles is a safe no-op.
static void test_cancel_invalid_handles() {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    // Must not crash or assert.
    actor->context()->cancel_schedule(AlarmHandle{});
    actor->context()->cancel_schedule(AlarmHandle{0});
}

// Test 3: cancelling before the timer fires prevents mailbox delivery.
//
// scheduler_threads=0 means no worker thread processes the mailbox,
// but the timer thread still runs and fires callbacks.  We schedule a
// message, cancel it, and then inspect the mailbox directly — the
// message must not be present.
static void test_cancel_prevents_mailbox_delivery() {
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system(config);

    auto handle = system.spawn<ScheduleTestActor>();
    auto actor = std::static_pointer_cast<ScheduleTestActor>(handle.get());

    auto alarm = actor->trigger_schedule(std::chrono::milliseconds(200));
    assert(alarm.id() != 0);

    // Cancel before the timer thread advances past the expiry.
    actor->context()->cancel_schedule(alarm);
    actor->context()->cancel_schedule(alarm); // double cancel is harmless

    // Wait long enough for the timer to have fired had it NOT been
    // cancelled.  The exact wait duration is not an assertion — it
    // merely gates the mailbox check below.
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Direct mailbox inspection — no worker thread involved.
    auto* mailbox = system.get_mailbox(actor->id());
    assert(mailbox != nullptr);

    TypedMessage msg;
    bool popped = mailbox->try_pop(msg);
    assert(!popped); // must NOT have been delivered
}

} // namespace

int main() {
    test_schedule_returns_valid_handle();
    test_cancel_invalid_handles();
    test_cancel_prevents_mailbox_delivery();
    return 0;
}
