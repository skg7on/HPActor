#include <hpactor/supervision/all_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>
#include <cassert>

void test_all_for_one_default() {
  hpactor::AllForOneSupervisor supervisor;
  hpactor::ChildFailure failure{hpactor::ActorId{1}, hpactor::error{1},
                                hpactor::SupervisionDirective::Stop};
  // AllForOne always returns Restart regardless of child directive
  auto directive = supervisor.on_child_failure(failure);
  assert(directive == hpactor::SupervisionDirective::Restart);
}

int main() {
  test_all_for_one_default();
  return 0;
}