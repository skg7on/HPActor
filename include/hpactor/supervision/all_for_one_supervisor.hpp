#pragma once
#include <hpactor/supervision/supervision.hpp>

namespace hpactor {

class AllForOneSupervisor : public Supervisor {
  public:
    explicit AllForOneSupervisor(SupervisionPolicy policy = {});
    SupervisionDirective on_child_failure(const ChildFailure& failure) override;

  private:
    SupervisionPolicy policy_;
};

} // namespace hpactor