#pragma once
#include <hpactor/supervision/supervision.hpp>

namespace hpactor {

class OneForOneSupervisor : public Supervisor {
  public:
    explicit OneForOneSupervisor(SupervisionPolicy policy = {});
    SupervisionDirective on_child_failure(const ChildFailure& failure) override;

  private:
    SupervisionPolicy policy_;
};

} // namespace hpactor