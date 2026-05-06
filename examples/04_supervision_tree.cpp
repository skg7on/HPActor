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
// HPActor Example 04: Supervision Tree
// =============================================================================
//
// Demonstrates fault-tolerance via supervision trees:
//
//   - SupervisorActor with OneForOne / AllForOne strategies
//   - Worker actors processing messages under supervision
//   - Child failure detection, restart counting, and rate limiting
//   - Restart with a spawn factory (RestartingSupervisor subclass)
//   - SelfSupervisingActor with custom on_failure() policy
//
// Supervision tree built in this example:
//
//   RestartingSupervisor (OneForOne)
//     ├── Worker-1
//     ├── Worker-2
//     └── Worker-3
//
//   SelfSupervisor (max_restarts=2, Escalate on failure)
//     └── Worker-4
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/messages.pb.h>
#include <hpactor/supervision/supervision.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/all_for_one_supervisor.hpp>

#include <cstring>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

// ---------------------------------------------------------------------------
// Message type tags
// ---------------------------------------------------------------------------

static const hpactor::TypeTag WorkTag{0x00001000};
static const hpactor::TypeTag CrashTag{0x00001001};
static const hpactor::TypeTag StatusTag{0x00001002};

// ---------------------------------------------------------------------------
// Payload helpers
// ---------------------------------------------------------------------------

static hpactor::StreamBuffer encode_int(int value) {
    hpactor::StreamBuffer payload(sizeof(int));
    std::memcpy(payload.data(), &value, sizeof(int));
    return payload;
}

static int decode_int(const hpactor::StreamBuffer& payload) {
    if (payload.size() < sizeof(int)) return 0;
    int value;
    std::memcpy(&value, payload.data(), sizeof(int));
    return value;
}

static hpactor::TypedMessage make_msg(hpactor::TypeTag tag, int value = 0) {
    return hpactor::TypedMessage(tag, encode_int(value));
}

// ---------------------------------------------------------------------------
// Build a DownMessage protobuf and wrap it as TypeTag::DownMsg
// ---------------------------------------------------------------------------

static hpactor::TypedMessage make_down_msg(uint64_t actor_id,
                                           uint32_t reason_code) {
    hpactor::DownMessage down;
    down.set_actor_id(actor_id);
    down.set_reason_code(reason_code);
    hpactor::StreamBuffer payload(down.ByteSizeLong());
    (void)down.SerializeToArray(payload.data(),
                                 static_cast<int>(payload.size()));
    return hpactor::TypedMessage(hpactor::TypeTag::DownMsg, std::move(payload));
}

// =============================================================================
// WorkerActor — processes work, can be told to crash
// =============================================================================

class WorkerActor : public hpactor::EventBasedActor {
  public:
    WorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                int worker_num)
        : hpactor::EventBasedActor(ctx, sys), worker_num_(worker_num) {
        become(make_behavior());
    }

    int worker_num() const { return worker_num_; }
    int work_done() const { return work_done_; }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == WorkTag) {
                int load = decode_int(msg.payload());
                work_done_ += load;
                std::cout << "  Worker-" << worker_num_ << " [" << id().value()
                          << "]: work +" << load << " (total=" << work_done_
                          << ")" << std::endl;
            } else if (msg.type_id() == CrashTag) {
                int code = decode_int(msg.payload());
                std::cout << "  Worker-" << worker_num_ << " [" << id().value()
                          << "]: CRASHING (code=" << code << ")" << std::endl;
            } else if (msg.type_id() == StatusTag) {
                std::cout << "  Worker-" << worker_num_ << " [" << id().value()
                          << "]: status — work_done=" << work_done_
                          << std::endl;
            }
        }};
    }

  private:
    int worker_num_;
    int work_done_ = 0;
};

// =============================================================================
// RestartingSupervisor — SupervisorActor that actually respawns children
// =============================================================================

class RestartingSupervisor : public hpactor::SupervisorActor {
  public:
    using Factory = std::function<hpactor::Actor()>;

    RestartingSupervisor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                         hpactor::Supervisor& strategy,
                         std::vector<hpactor::Actor> children, Factory factory)
        : hpactor::SupervisorActor(ctx, sys, strategy, std::move(children)),
          factory_(std::move(factory)) {}

    const hpactor::SupervisorActor::ActorVec& children() const {
        return children_;
    }

  protected:
    void restart_child(hpactor::ActorId child_id) override {
        // Remove the dead child from our list
        children_.erase(std::remove_if(children_.begin(), children_.end(),
                                       [&child_id](const hpactor::Actor& a) {
                                           return a.id() == child_id;
                                       }),
                        children_.end());

        // Let the base manage restart counts and the sliding window
        hpactor::SupervisorActor::restart_child(child_id);

        // Spawn a fresh replacement via the factory
        hpactor::Actor new_child = factory_();
        children_.push_back(new_child);

        std::cout << "  [Supervisor]: restarted child → new id="
                  << new_child.id().value() << std::endl;
    }

  private:
    Factory factory_;
};

// =============================================================================
// SelfSupervisor — SelfSupervisingActor with custom on_failure policy
// =============================================================================

class SelfSupervisor : public hpactor::SelfSupervisingActor {
  public:
    SelfSupervisor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                   hpactor::SupervisionPolicy policy)
        : hpactor::SelfSupervisingActor(ctx, sys, std::move(policy)) {
        become(make_behavior());
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == hpactor::TypeTag::DownMsg) {
                handle_child_down(msg.type_id(), msg.payload());
            }
        }};
    }

    hpactor::SupervisionDirective
    on_failure(hpactor::ActorId child_id,
               const hpactor::error& err) override {
        std::cout << "  [SelfSupervisor]: child " << child_id.value()
                  << " failed (code=" << err.code() << ")"
                  << " — escalating" << std::endl;
        return hpactor::SupervisionDirective::Escalate;
    }
};

// ---------------------------------------------------------------------------
// send_from_main
// ---------------------------------------------------------------------------

static void send_from_main(hpactor::ActorSystem& system,
                           hpactor::ActorId target, hpactor::TypeTag tag,
                           int value = 0) {
    system.deliver_local(target, make_msg(tag, value));
}

// =============================================================================
// Main
// =============================================================================

int main() {
    std::cout << "=== HPActor Example 04: Supervision Tree ===" << std::endl;

    hpactor::Config config{.scheduler_threads = 2, .max_queue_depth = 1024, .cli = {}};
    hpactor::ActorSystem system(config);

    // ---- Setup: OneForOne Supervisor with 3 workers ----
    hpactor::OneForOneSupervisor one_for_one;

    // Track worker IDs so we can address them from main
    std::vector<hpactor::ActorId> worker_ids;

    int worker_counter = 0;
    auto factory = [&]() -> hpactor::Actor {
        ++worker_counter;
        auto w = system.spawn<WorkerActor>(worker_counter);
        worker_ids.push_back(w.id());
        return w;
    };

    std::vector<hpactor::Actor> workers;
    for (int i = 1; i <= 3; ++i) {
        workers.push_back(factory());
    }

    auto supervisor = system.spawn<RestartingSupervisor>(
        one_for_one, std::move(workers), factory);
    std::cout << "Spawned RestartingSupervisor (id=" << supervisor.id().value()
              << ", OneForOne, 3 workers: ids=" << worker_ids[0].value()
              << "," << worker_ids[1].value() << "," << worker_ids[2].value()
              << ")" << std::endl;

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // ---- Demo 1: Workers process work ----
    std::cout << "\n--- Demo 1: Workers process work ---" << std::endl;
    send_from_main(system, worker_ids[0], WorkTag, 10);
    send_from_main(system, worker_ids[1], WorkTag, 20);
    send_from_main(system, worker_ids[2], WorkTag, 30);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Demo 2: OneForOne — crash worker-2, supervisor restarts ----
    std::cout << "\n--- Demo 2: OneForOne restart ---" << std::endl;
    // Simulate crash
    send_from_main(system, worker_ids[1], CrashTag, 42);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    // Deliver DownMsg to supervisor (framework would do this automatically
    // when failure detection is wired)
    system.deliver_local(supervisor.id(),
                         make_down_msg(worker_ids[1].value(), 42));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    // The replacement worker should now be at worker_ids[3]
    if (worker_ids.size() > 3) {
        send_from_main(system, worker_ids[3], WorkTag, 15);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Demo 3: SelfSupervisingActor with custom policy ----
    std::cout << "\n--- Demo 3: SelfSupervisingActor (Escalate on failure) ---"
              << std::endl;
    hpactor::SupervisionPolicy strict_policy;
    strict_policy.max_restarts = 2;
    strict_policy.restart_interval = std::chrono::milliseconds{5000};

    auto self_sup = system.spawn<SelfSupervisor>(strict_policy);
    std::cout << "Spawned SelfSupervisor (id=" << self_sup.id().value()
              << ", max_restarts=2, on_failure → Escalate)" << std::endl;

    auto worker4 = factory();
    auto worker4_id = worker4.id();
    std::cout << "  Added Worker-" << worker_counter << " (id="
              << worker4_id.value() << ")" << std::endl;
    send_from_main(system, worker4_id, WorkTag, 5);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Simulate rapid failures — SelfSupervisor's decide_restart rate-limits
    std::cout << "\n  Simulating 3 rapid failures (max_restarts=2)..."
              << std::endl;
    for (int i = 0; i < 3; ++i) {
        std::cout << "  Failure #" << (i + 1) << ": ";
        system.deliver_local(self_sup.id(),
                             make_down_msg(worker4_id.value(), 99));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // ---- Summary ----
    std::cout << "\n--- Supervision API reference ---" << std::endl;
    std::cout << "  SupervisorActor(ctx, sys, strategy, children)" << std::endl;
    std::cout << "    restart_child(child_id)  // virtual, override for spawn"
              << std::endl;
    std::cout << "  OneForOneSupervisor  — echoes failure directive" << std::endl;
    std::cout << "  AllForOneSupervisor  — always returns Restart" << std::endl;
    std::cout << "  SelfSupervisingActor — owns children, virtual on_failure()"
              << std::endl;
    std::cout << "  SupervisionPolicy{max_restarts, restart_interval}"
              << std::endl;

    std::cout << "\n=== Complete ===" << std::endl;
    return 0;
}
