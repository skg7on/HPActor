#pragma once
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types.hpp>
#include <chrono>
#include <unordered_map>
#include <vector>

namespace hpactor {

// SupervisionDirective - directive from supervisor to child on failure
enum class SupervisionDirective { Restart, Stop, Escalate };

// ChildFailure - information about a child actor failure
struct ChildFailure {
    ActorId child_id;
    error reason;
    SupervisionDirective directive;
};

// SupervisionPolicy - configuration for supervision behavior
struct SupervisionPolicy {
    enum class Strategy { OneForOne, AllForOne, OneForAll };
    Strategy strategy = Strategy::OneForOne;
    uint32_t max_restarts = 10;
    std::chrono::milliseconds restart_interval{5000};
};

// Supervisor - interface for supervision strategy
class Supervisor {
public:
    virtual ~Supervisor() = default;
    virtual SupervisionDirective on_child_failure(const ChildFailure& failure) = 0;
    virtual void on_child_stopped(ActorId child_id);
};

} // namespace hpactor