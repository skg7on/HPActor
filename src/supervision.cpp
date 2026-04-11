#include <hpactor/supervision/supervision.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/all_for_one_supervisor.hpp>

namespace hpactor {

void Supervisor::on_child_stopped(ActorId /*child_id*/) {
  // Default implementation does nothing
}

OneForOneSupervisor::OneForOneSupervisor(SupervisionPolicy policy)
    : policy_(std::move(policy)) {}

SupervisionDirective OneForOneSupervisor::on_child_failure(const ChildFailure& failure) {
  // OneForOne: only the failed child is affected
  return failure.directive;
}

AllForOneSupervisor::AllForOneSupervisor(SupervisionPolicy policy)
    : policy_(std::move(policy)) {}

SupervisionDirective AllForOneSupervisor::on_child_failure(const ChildFailure& /*failure*/) {
  // AllForOne: failure affects all children, restart all
  return SupervisionDirective::Restart;
}

} // namespace hpactor