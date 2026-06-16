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
// HPActor Example 13: EDF (Earliest Deadline First) Scheduling Demo
// =============================================================================
//
// Demonstrates opt-in EDF scheduling where messages with explicit deadlines
// are dispatched in earliest-deadline-first order, while ordinary messages
// stay on the priority-only fast path.
//
// Key concepts:
//
//   - deliver_local_edf()  — send with explicit deadline from outside
//   - context()->send_edf() — send with explicit deadline from within an actor
//   - EDF ordering         — earlier deadline dispatched before later deadline
//   - Priority tiebreaker  — within the same deadline, higher priority wins
//   - EDF vs priority mix  — EDF items dispatched before priority-only items
//   - Zero overhead        — ordinary send() never touches the EDF path
//
// Architecture:
//
//   Main ──(deliver_local_edf)──> AlarmActor-1 (deadline +1ms, emerg)
//        ──(deliver_local_edf)──> AlarmActor-2 (deadline +5ms, normal)
//        ──(deliver_local)──────> LogActor     (no deadline, background)
//
//   Expected dispatch order: AlarmActor-1 → AlarmActor-2 → LogActor
//
// =============================================================================

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Message type tags (user range)
// ---------------------------------------------------------------------------

static const hpactor::TypeTag AlarmTriggeredTag{0x00002000};
static const hpactor::TypeTag LogEventTag{0x00002001};

// ---------------------------------------------------------------------------
// Payload helpers
// ---------------------------------------------------------------------------

static hpactor::StreamBuffer encode_str(const std::string& s) {
    hpactor::StreamBuffer payload(s.size() + 1);
    std::memcpy(payload.data(), s.c_str(), s.size() + 1);
    return payload;
}

static std::string decode_str(const hpactor::StreamBuffer& payload) {
    return std::string(reinterpret_cast<const char*>(payload.data()));
}

// ---------------------------------------------------------------------------
// Shared dispatch-order recorder (thread-safe)
// ---------------------------------------------------------------------------

class DispatchLog {
  public:
    void record(const std::string& entry) {
        std::lock_guard<std::mutex> lock(mutex_);
        entries_.push_back(entry);
    }

    void print() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::cout << "\n── Dispatch Order ──────────────────────────────\n";
        for (size_t i = 0; i < entries_.size(); ++i) {
            std::cout << "  [" << (i + 1) << "] " << entries_[i] << "\n";
        }
        std::cout << "────────────────────────────────────────────────\n";
    }

  private:
    mutable std::mutex mutex_;
    std::vector<std::string> entries_;
};

// ---------------------------------------------------------------------------
// AlarmActor — handles time-critical alarm messages with EDF scheduling
// ---------------------------------------------------------------------------

class AlarmActor : public hpactor::EventBasedActor {
  public:
    AlarmActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
               std::string name, DispatchLog* log)
        : hpactor::EventBasedActor(ctx, sys) {
        name_ = std::move(name);
        log_ = log;
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != AlarmTriggeredTag)
                return;
            auto detail = decode_str(msg.payload());
            std::ostringstream oss;
            oss << name_ << " handled ALARM [deadline=" << msg.deadline_ns()
                << " ns]: " << detail;
            log_->record(oss.str());
            std::cout << "  \033[1;31m" << oss.str() << "\033[0m\n";

            if (forward_id_ != hpactor::ActorId{0}) {
                hpactor::TypedMessage fwd(AlarmTriggeredTag,
                                          encode_str("forwarded from " + name_));
                context()->send_edf(
                    hpactor::ActorAddress{{}, hpactor::ActorType{0}, forward_id_, 0},
                    std::move(fwd), std::chrono::milliseconds(2),
                    /*priority=*/0);
            }
        }};
    }

  public:
    void set_forward_id(hpactor::ActorId id) {
        forward_id_ = id;
    }

  private:
    std::string name_;
    DispatchLog* log_ = nullptr;
    hpactor::ActorId forward_id_;
};

// ---------------------------------------------------------------------------
// LogActor — handles background logging (no deadline, priority-only path)
// ---------------------------------------------------------------------------

class LogActor : public hpactor::EventBasedActor {
  public:
    LogActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
             std::string name, DispatchLog* log)
        : hpactor::EventBasedActor(ctx, sys) {
        name_ = std::move(name);
        log_ = log;
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != LogEventTag)
                return;
            auto detail = decode_str(msg.payload());
            std::ostringstream oss;
            oss << name_ << " logged: " << detail << " [priority-only, no EDF]";
            log_->record(oss.str());
            std::cout << "  \033[2m" << oss.str() << "\033[0m\n";
        }};
    }

  private:
    std::string name_;
    DispatchLog* log_ = nullptr;
};

// ---------------------------------------------------------------------------
// Demo 1: Basic EDF ordering — earlier deadline dispatched first
// ---------------------------------------------------------------------------

static void demo_basic_edf_ordering(hpactor::ActorSystem& system) {
    std::cout << "\n=== Demo 1: Basic EDF Ordering ===\n";
    std::cout << "NormalAlarm (deadline +10ms) enqueued FIRST.\n";
    std::cout << "UrgentAlarm (deadline +1ms)  enqueued SECOND.\n";
    std::cout << "UrgentAlarm should dispatch first (earlier deadline).\n\n";

    DispatchLog log;
    auto urgent = system.spawn<AlarmActor>("UrgentAlarm", &log);
    auto normal = system.spawn<AlarmActor>("NormalAlarm", &log);

    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

    // Enqueue NormalAlarm first (later deadline).
    system.deliver_local_edf(
        normal.id(),
        hpactor::TypedMessage(AlarmTriggeredTag, encode_str("routine check")),
        now + 10'000'000, 0);

    // Enqueue UrgentAlarm second (earlier deadline).
    system.deliver_local_edf(
        urgent.id(),
        hpactor::TypedMessage(AlarmTriggeredTag,
                              encode_str("CRITICAL: temperature spike")),
        now + 1'000'000, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log.print();
}

// ---------------------------------------------------------------------------
// Demo 2: EDF + priority tiebreaker
// ---------------------------------------------------------------------------

static void demo_edf_priority_tiebreaker(hpactor::ActorSystem& system) {
    std::cout << "\n=== Demo 2: EDF Priority Tiebreaker ===\n";
    std::cout << "Both alarms get the SAME deadline (+5ms).\n";
    std::cout << "HighPriorityAlarm uses priority=0 (highest).\n";
    std::cout << "LowPriorityAlarm  uses priority=3 (lowest).\n\n";

    DispatchLog log;
    auto low = system.spawn<AlarmActor>("LowPriorityAlarm", &log);
    auto high = system.spawn<AlarmActor>("HighPriorityAlarm", &log);

    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    int64_t shared = now + 5'000'000;

    // Low priority enqueued first.
    system.deliver_local_edf(
        low.id(),
        hpactor::TypedMessage(AlarmTriggeredTag, encode_str("low-priority check")),
        shared, 3);

    // High priority enqueued second (same deadline).
    system.deliver_local_edf(
        high.id(),
        hpactor::TypedMessage(AlarmTriggeredTag, encode_str("high-priority alert")),
        shared, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log.print();
}

// ---------------------------------------------------------------------------
// Demo 3: EDF takes precedence over regular priority-only messages
// ---------------------------------------------------------------------------

static void demo_edf_mixed_with_priority(hpactor::ActorSystem& system) {
    std::cout << "\n=== Demo 3: EDF + Priority Items Enqueued Together ===\n";
    std::cout << "Background log enqueued FIRST (no EDF deadline).\n";
    std::cout << "EDF alarm   enqueued immediately after (+1ms EDF deadline).\n";
    std::cout << "When both are in the scheduler queues simultaneously,\n";
    std::cout << "the EDF item is dispatched first by pop_edf().\n\n";

    DispatchLog log;
    auto logger = system.spawn<LogActor>("BackgroundLogger", &log);
    auto alarm = system.spawn<AlarmActor>("EDFAlarm", &log);

    // Enqueue background log first (priority-only, no EDF).
    system.deliver_local(
        logger.id(),
        hpactor::TypedMessage(LogEventTag, encode_str("system startup complete")));

    // Enqueue alarm immediately after (EDF with tight deadline).
    // With both items in the scheduler queues simultaneously,
    // the EDF item should be dispatched first by pop_edf().
    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    system.deliver_local_edf(
        alarm.id(),
        hpactor::TypedMessage(AlarmTriggeredTag, encode_str("immediate action")),
        now + 1'000'000, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log.print();
}

// ---------------------------------------------------------------------------
// Demo 4: send_edf() from within an actor (forwarding chain)
// ---------------------------------------------------------------------------

static void demo_send_edf_from_actor(hpactor::ActorSystem& system) {
    std::cout << "\n=== Demo 4: send_edf() From Within an Actor ===\n";
    std::cout << "PrimaryAlarm receives EDF message and forwards\n";
    std::cout << "to SecondaryAlarm using context()->send_edf().\n\n";

    DispatchLog log;
    auto primary = system.spawn<AlarmActor>("PrimaryAlarm", &log);
    auto secondary = system.spawn<AlarmActor>("SecondaryAlarm", &log);

    // Wire the forwarding chain.
    // Use Actor::operator->() to get the underlying AbstractActor.
    static_cast<AlarmActor*>(primary.get().get())->set_forward_id(secondary.id());

    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    system.deliver_local_edf(
        primary.id(),
        hpactor::TypedMessage(AlarmTriggeredTag, encode_str("initial trigger")),
        now + 1'000'000, 0);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log.print();
}

// ---------------------------------------------------------------------------
// Demo 5: EDF is opt-in — regular send() does NOT use EDF
// ---------------------------------------------------------------------------

static void demo_edf_is_opt_in(hpactor::ActorSystem& system) {
    std::cout << "\n=== Demo 5: EDF Is Opt-In ===\n";
    std::cout << "Only deliver_local_edf() triggers EDF placement.\n";
    std::cout << "Regular deliver_local() uses priority-only path.\n\n";

    DispatchLog log;
    auto alarm = system.spawn<AlarmActor>("OptInAlarm", &log);

    // Regular delivery (priority-only, no EDF).
    system.deliver_local(
        alarm.id(), hpactor::TypedMessage(AlarmTriggeredTag,
                                          encode_str("regular message (no EDF)")));

    // EDF delivery.
    int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    system.deliver_local_edf(
        alarm.id(),
        hpactor::TypedMessage(AlarmTriggeredTag, encode_str("EDF-scheduled message")),
        now + 1'000'000, 0);

    // Another regular delivery.
    system.deliver_local(
        alarm.id(), hpactor::TypedMessage(AlarmTriggeredTag,
                                          encode_str("another regular message")));

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    log.print();
}

// ---------------------------------------------------------------------------
// main
// ---------------------------------------------------------------------------

int main() {
    std::cout << "╔══════════════════════════════════════════════════╗\n";
    std::cout << "║  HPActor EDF Scheduling Demo                     ║\n";
    std::cout << "╚══════════════════════════════════════════════════╝\n";
    std::cout << "\nOpt-in EDF scheduling: messages with explicit\n";
    std::cout << "deadlines are dispatched in earliest-deadline-first\n";
    std::cout << "order. Ordinary messages stay on the priority-only\n";
    std::cout << "fast path.\n";

    // ── Create the actor system ───────────────────────────────────────────
    hpactor::Config cfg;
    cfg.scheduler_threads = 2;
    cfg.enable_network = false;
    hpactor::ActorSystem system(cfg);

    std::cout << "\nActorSystem started with " << cfg.scheduler_threads
              << " worker threads.\n";

    // ── Run demos ─────────────────────────────────────────────────────────
    demo_basic_edf_ordering(system);
    demo_edf_priority_tiebreaker(system);
    demo_edf_mixed_with_priority(system);
    demo_send_edf_from_actor(system);
    demo_edf_is_opt_in(system);

    // ── Shutdown ──────────────────────────────────────────────────────────
    std::cout << "\n── All demos complete. Shutting down. ──────────\n";
    hpactor::ShutdownOptions opts;
    opts.ingress_timeout = std::chrono::milliseconds(100);
    opts.actor_drain_timeout = std::chrono::milliseconds(100);
    opts.cluster_leave_timeout = std::chrono::milliseconds(100);
    system.shutdown(opts);

    std::cout << "Done.\n";
    return 0;
}
