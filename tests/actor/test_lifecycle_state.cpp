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

#define TEST(name) static void name()
#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::cerr << "FAIL: " << #cond << " at " << __FILE__ << ":"        \
                      << __LINE__ << '\n';                                     \
            std::abort();                                                      \
        }                                                                      \
    } while (0)
#define CHECK_EQ(a, b) CHECK((a) == (b))

TEST(test_default_state_is_starting) {
    TestLifecycleActor a;
    CHECK_EQ(a.state(), LifecycleState::kStarting);
    CHECK_EQ(std::string(a.state_string()), "starting");
    std::cout << "PASS: test_default_state_is_starting\n";
}

TEST(test_starting_to_active) {
    TestLifecycleActor a;
    bool ok = a.transition(LifecycleState::kActive);
    CHECK(ok);
    CHECK_EQ(a.state(), LifecycleState::kActive);
    CHECK_EQ(a.start_calls, 1);
    std::cout << "PASS: test_starting_to_active\n";
}

TEST(test_active_to_starting_illegal) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kStarting);
    CHECK(!ok);
    CHECK_EQ(a.state(), LifecycleState::kActive);
    std::cout << "PASS: test_active_to_starting_illegal\n";
}

TEST(test_active_to_recovering_illegal) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    bool ok = a.transition(LifecycleState::kRecovering);
    CHECK(!ok);
    CHECK_EQ(a.state(), LifecycleState::kActive);
    std::cout << "PASS: test_active_to_recovering_illegal\n";
}

TEST(test_full_happy_path) {
    TestLifecycleActor a;
    CHECK(a.transition(LifecycleState::kActive));
    CHECK_EQ(a.start_calls, 1);
    CHECK(a.transition(LifecycleState::kDraining));
    CHECK_EQ(a.drain_calls, 1);
    CHECK(a.transition(LifecycleState::kStopping));
    CHECK_EQ(a.stop_calls, 1);
    CHECK(a.transition(LifecycleState::kStopped));
    CHECK_EQ(a.deactivate_calls, 1);
    CHECK_EQ(a.state(), LifecycleState::kStopped);
    std::cout << "PASS: test_full_happy_path\n";
}

TEST(test_failure_restart_path) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    CHECK(a.transition(LifecycleState::kFailed));
    CHECK_EQ(a.fail_calls, 1);
    CHECK_EQ(a.state(), LifecycleState::kFailed);
    a.bump_incarnation();
    CHECK(a.transition(LifecycleState::kStarting));
    CHECK_EQ(a.restart_calls, 1);
    CHECK(a.transition(LifecycleState::kActive));
    CHECK_EQ(a.start_calls, 1);
    std::cout << "PASS: test_failure_restart_path\n";
}

TEST(test_recovery_path) {
    TestLifecycleActor a;
    a.transition(LifecycleState::kActive);
    a.transition(LifecycleState::kFailed);
    CHECK(a.transition(LifecycleState::kRecovering));
    CHECK_EQ(a.recover_calls, 1);
    CHECK(a.transition(LifecycleState::kActive));
    CHECK_EQ(a.start_calls, 1);
    std::cout << "PASS: test_recovery_path\n";
}

TEST(test_state_string) {
    TestLifecycleActor a;
    CHECK_EQ(std::string(a.state_string()), "starting");
    a.transition(LifecycleState::kActive);
    CHECK_EQ(std::string(a.state_string()), "active");
    a.transition(LifecycleState::kDraining);
    CHECK_EQ(std::string(a.state_string()), "draining");
    a.transition(LifecycleState::kStopping);
    CHECK_EQ(std::string(a.state_string()), "stopping");
    a.transition(LifecycleState::kStopped);
    CHECK_EQ(std::string(a.state_string()), "stopped");
    std::cout << "PASS: test_state_string\n";
}

TEST(test_accepts_user_msgs) {
    TestLifecycleActor a;
    CHECK(!a.accepts_user_msgs()); // STARTING: false
    a.transition(LifecycleState::kActive);
    CHECK(a.accepts_user_msgs()); // ACTIVE: true
    a.transition(LifecycleState::kDraining);
    CHECK(!a.accepts_user_msgs()); // DRAINING: false
    std::cout << "PASS: test_accepts_user_msgs\n";
}

TEST(test_accepts_system_msgs) {
    TestLifecycleActor a;
    CHECK(a.accepts_system_msgs()); // STARTING: true
    a.transition(LifecycleState::kActive);
    CHECK(a.accepts_system_msgs()); // ACTIVE: true
    a.transition(LifecycleState::kDraining);
    CHECK(a.accepts_system_msgs()); // DRAINING: true
    a.transition(LifecycleState::kStopping);
    CHECK(a.accepts_system_msgs()); // STOPPING: true
    a.transition(LifecycleState::kStopped);
    CHECK(!a.accepts_system_msgs()); // STOPPED: false
    std::cout << "PASS: test_accepts_system_msgs\n";
}

TEST(test_transition_invokes_correct_hook) {
    TestLifecycleActor a;
    CHECK(a.transition(LifecycleState::kActive));
    CHECK_EQ(a.start_calls, 1);
    CHECK_EQ(a.drain_calls, 0);
    CHECK(a.transition(LifecycleState::kDraining));
    CHECK_EQ(a.drain_calls, 1);
    CHECK(a.transition(LifecycleState::kStopping));
    CHECK_EQ(a.stop_calls, 1);
    CHECK(a.transition(LifecycleState::kStopped));
    CHECK_EQ(a.deactivate_calls, 1);
    CHECK_EQ(a.fail_calls, 0);
    CHECK_EQ(a.recover_calls, 0);
    CHECK_EQ(a.restart_calls, 0);
    std::cout << "PASS: test_transition_invokes_correct_hook\n";
}

TEST(test_incarnation_bumps) {
    TestLifecycleActor a;
    CHECK_EQ(a.incarnation(), 0);
    a.bump_incarnation();
    CHECK_EQ(a.incarnation(), 1);
    a.bump_incarnation();
    CHECK_EQ(a.incarnation(), 2);
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
