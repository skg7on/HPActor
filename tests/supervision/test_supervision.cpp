#include <hpactor/supervision/supervision.hpp>
#include <cassert>

void test_supervision_directive_values() {
    assert(static_cast<int>(hpactor::SupervisionDirective::Restart) == 0);
    assert(static_cast<int>(hpactor::SupervisionDirective::Stop) == 1);
    assert(static_cast<int>(hpactor::SupervisionDirective::Escalate) == 2);
}

void test_child_failure_struct() {
    hpactor::ActorId id(42);
    hpactor::ChildFailure failure{
        id,
        hpactor::error{1, "test error"},
        hpactor::SupervisionDirective::Restart
    };
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

int main() {
    test_supervision_directive_values();
    test_child_failure_struct();
    test_supervision_policy_default();
    test_supervision_policy_custom();
    return 0;
}