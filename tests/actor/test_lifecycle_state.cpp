// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/actor/lifecycle_actor.hpp>
#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <iostream>

using namespace hpactor;

// Test harness: LifecycleActor with tracking
class TestLifecycleActor : public LifecycleActor {
  public:
    int start_calls = 0;
    int drain_calls = 0;
    int stop_calls = 0;
    int deactivate_calls = 0;
    int fail_calls = 0;
    int recover_calls = 0;
    int restart_calls = 0;

    void on_start() override {
        start_calls++;
    }
    void on_drain() override {
        drain_calls++;
    }
    void on_stop() override {
        stop_calls++;
    }
    void on_deactivate() override {
        deactivate_calls++;
    }
    void on_fail(error) override {
        fail_calls++;
    }
    void on_recover() override {
        recover_calls++;
    }
    void on_restart() override {
        restart_calls++;
    }
};

static void test_default_state_is_starting() {
    TestLifecycleActor a;
    assert(a.state() == LifecycleState::kStarting);
    assert(std::string(a.state_string()) == "starting");
    std::cout << "PASS: test_default_state_is_starting\n";
}

static void test_starting_to_active() {
    TestLifecycleActor a;
    bool ok = a.transition(LifecycleState::kActive);
    assert(ok);
    assert(a.state() == LifecycleState::kActive);
    assert(a.start_calls == 1);
    std::cout << "PASS: test_starting_to_active\n";
}

static void test_active_to_starting_illegal() {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kStarting);
    assert(!ok);
    assert(a.state() == LifecycleState::kActive);
    std::cout << "PASS: test_active_to_starting_illegal\n";
}

static void test_active_to_recovering_illegal() {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kRecovering);
    assert(!ok);
    assert(a.state() == LifecycleState::kActive);
    std::cout << "PASS: test_active_to_recovering_illegal\n";
}

static void test_full_happy_path() {
    TestLifecycleActor a;
    assert(a.transition(LifecycleState::kActive));
    assert(a.start_calls == 1);
    assert(a.transition(LifecycleState::kDraining));
    assert(a.drain_calls == 1);
    assert(a.transition(LifecycleState::kStopping));
    assert(a.stop_calls == 1);
    assert(a.transition(LifecycleState::kStopped));
    assert(a.deactivate_calls == 1);
    assert(a.state() == LifecycleState::kStopped);
    std::cout << "PASS: test_full_happy_path\n";
}

static void test_failure_restart_path() {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    assert(a.transition(LifecycleState::kFailed));
    assert(a.fail_calls == 1);
    assert(a.state() == LifecycleState::kFailed);
    a.bump_incarnation();
    assert(a.transition(LifecycleState::kStarting));
    assert(a.restart_calls == 1);
    assert(a.transition(LifecycleState::kActive));
    assert(a.start_calls == 2);
    std::cout << "PASS: test_failure_restart_path\n";
}

static void test_recovery_path() {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kFailed);
    assert(a.transition(LifecycleState::kRecovering));
    assert(a.recover_calls == 1);
    assert(a.transition(LifecycleState::kActive));
    assert(a.start_calls == 2); // RECOVERING→ACTIVE also fires on_start
    std::cout << "PASS: test_recovery_path\n";
}

static void test_state_string() {
    TestLifecycleActor a;
    assert(std::string(a.state_string()) == "starting");
    a.transition(LifecycleState::kActive);
    assert(std::string(a.state_string()) == "active");
    a.transition(LifecycleState::kDraining);
    assert(std::string(a.state_string()) == "draining");
    a.transition(LifecycleState::kStopping);
    assert(std::string(a.state_string()) == "stopping");
    a.transition(LifecycleState::kStopped);
    assert(std::string(a.state_string()) == "stopped");
    std::cout << "PASS: test_state_string\n";
}

static void test_accepts_user_msgs() {
    TestLifecycleActor a;
    assert(!a.accepts_user_msgs());
    a.transition(LifecycleState::kActive);
    assert(a.accepts_user_msgs());
    a.transition(LifecycleState::kDraining);
    assert(!a.accepts_user_msgs());
    std::cout << "PASS: test_accepts_user_msgs\n";
}

static void test_accepts_system_msgs() {
    TestLifecycleActor a;
    assert(a.accepts_system_msgs());
    a.transition(LifecycleState::kActive);
    assert(a.accepts_system_msgs());
    a.transition(LifecycleState::kDraining);
    assert(a.accepts_system_msgs());
    a.transition(LifecycleState::kStopping);
    assert(a.accepts_system_msgs());
    a.transition(LifecycleState::kStopped);
    assert(!a.accepts_system_msgs());
    std::cout << "PASS: test_accepts_system_msgs\n";
}

static void test_transition_invokes_correct_hook() {
    TestLifecycleActor a;
    assert(a.transition(LifecycleState::kActive));
    assert(a.start_calls == 1);
    assert(a.drain_calls == 0);
    assert(a.transition(LifecycleState::kDraining));
    assert(a.drain_calls == 1);
    assert(a.transition(LifecycleState::kStopping));
    assert(a.stop_calls == 1);
    assert(a.transition(LifecycleState::kStopped));
    assert(a.deactivate_calls == 1);
    assert(a.fail_calls == 0);
    assert(a.recover_calls == 0);
    assert(a.restart_calls == 0);
    std::cout << "PASS: test_transition_invokes_correct_hook\n";
}

static void test_incarnation_bumps() {
    TestLifecycleActor a;
    assert(a.incarnation() == 0);
    a.bump_incarnation();
    assert(a.incarnation() == 1);
    a.bump_incarnation();
    assert(a.incarnation() == 2);
    std::cout << "PASS: test_incarnation_bumps\n";
}

int main() {
    test_default_state_is_starting();
    test_starting_to_active();
    test_active_to_starting_illegal();
    test_active_to_recovering_illegal();
    test_full_happy_path();
    test_failure_restart_path();
    test_recovery_path();
    test_state_string();
    test_accepts_user_msgs();
    test_accepts_system_msgs();
    test_transition_invokes_correct_hook();
    test_incarnation_bumps();
    std::cout << "\nAll 12 lifecycle state tests passed.\n";
    return 0;
}
