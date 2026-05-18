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

#include <cassert>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/supervision/all_for_one_supervisor.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>

void test_supervision_directive_values() {
    assert(static_cast<int>(hpactor::SupervisionDirective::Restart) == 0);
    assert(static_cast<int>(hpactor::SupervisionDirective::Stop) == 1);
    assert(static_cast<int>(hpactor::SupervisionDirective::Escalate) == 2);
}

void test_child_failure_struct() {
    hpactor::ActorId id(42);
    hpactor::ChildFailure failure{id, hpactor::error{1, "test error"},
                                  hpactor::SupervisionDirective::Restart};
    assert(failure.child_id.value() == 42);
    assert(failure.reason.code() == 1);
    assert(failure.directive == hpactor::SupervisionDirective::Restart);
}

void test_supervision_policy_default() {
    hpactor::SupervisionPolicy policy;
    assert(policy.strategy == hpactor::SupervisionPolicy::Strategy::OneForOne);
    assert(policy.max_restarts == 10);
    assert(policy.restart_interval.count() == 5000);
}

void test_supervision_policy_custom() {
    hpactor::SupervisionPolicy policy;
    policy.strategy = hpactor::SupervisionPolicy::Strategy::AllForOne;
    policy.max_restarts = 5;
    policy.restart_interval = std::chrono::milliseconds(1000);
    assert(policy.strategy == hpactor::SupervisionPolicy::Strategy::AllForOne);
    assert(policy.max_restarts == 5);
    assert(policy.restart_interval.count() == 1000);
}

void test_one_for_one_passes_directive() {
    hpactor::SupervisionPolicy policy;
    hpactor::OneForOneSupervisor sup(policy);
    hpactor::ChildFailure failure;
    failure.child_id = hpactor::ActorId{1};
    failure.reason = hpactor::error(0);
    failure.directive = hpactor::SupervisionDirective::Restart;
    assert(sup.on_child_failure(failure) == hpactor::SupervisionDirective::Restart);
    failure.directive = hpactor::SupervisionDirective::Stop;
    assert(sup.on_child_failure(failure) == hpactor::SupervisionDirective::Stop);
    failure.directive = hpactor::SupervisionDirective::Escalate;
    assert(sup.on_child_failure(failure) == hpactor::SupervisionDirective::Escalate);
    printf("  PASSED test_one_for_one_passes_directive\n");
}

void test_all_for_one_always_restart() {
    hpactor::SupervisionPolicy policy;
    hpactor::AllForOneSupervisor sup(policy);
    hpactor::ChildFailure failure;
    failure.child_id = hpactor::ActorId{1};
    failure.reason = hpactor::error(0);
    failure.directive = hpactor::SupervisionDirective::Stop;
    assert(sup.on_child_failure(failure) == hpactor::SupervisionDirective::Restart);
    printf("  PASSED test_all_for_one_always_restart\n");
}

void test_supervisor_actor_construct() {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(cfg);
    hpactor::OneForOneSupervisor strategy(hpactor::SupervisionPolicy{});
    std::vector<hpactor::Actor> children;
    hpactor::SupervisorActor actor(nullptr, sys, strategy, std::move(children));
    (void)actor;
    printf("  PASSED test_supervisor_actor_construct\n");
}

void test_self_supervising_construct() {
    hpactor::Config cfg;
    cfg.scheduler_threads = 0;
    hpactor::ActorSystem sys(cfg);
    hpactor::SupervisionPolicy policy;
    hpactor::SelfSupervisingActor actor(nullptr, sys, policy);
    (void)actor;
    printf("  PASSED test_self_supervising_construct\n");
}

int main() {
    test_supervision_directive_values();
    test_child_failure_struct();
    test_supervision_policy_default();
    test_supervision_policy_custom();
    test_one_for_one_passes_directive();
    test_all_for_one_always_restart();
    test_supervisor_actor_construct();
    test_self_supervising_construct();
    return 0;
}