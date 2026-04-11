#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>
#include <cassert>

void test_one_for_one_default_policy() {
  hpactor::OneForOneSupervisor supervisor;
  hpactor::ChildFailure failure{hpactor::ActorId{1}, hpactor::error{1},
                                hpactor::SupervisionDirective::Restart};
  auto directive = supervisor.on_child_failure(failure);
  assert(directive == hpactor::SupervisionDirective::Restart);
}

void test_one_for_one_custom_policy() {
  hpactor::SupervisionPolicy policy;
  policy.max_restarts = 5;
  hpactor::OneForOneSupervisor supervisor(policy);
  hpactor::ChildFailure failure{hpactor::ActorId{1}, hpactor::error{1},
                                hpactor::SupervisionDirective::Stop};
  auto directive = supervisor.on_child_failure(failure);
  assert(directive == hpactor::SupervisionDirective::Stop);
}

int main() {
  test_one_for_one_default_policy();
  test_one_for_one_custom_policy();
  return 0;
}