// =============================================================================
// HPActor Example 04: Supervision Tree
// =============================================================================
//
// This example demonstrates hierarchical supervision in HPActor.
//
// Key concepts demonstrated:
//   - hpactor::SupervisionPolicy with OneForOne and AllForOne strategies
//   - hpactor::SupervisorActor that supervises child actors
//   - hpactor::SelfSupervisingActor for self-supervised children
//   - Child actor registration and down_msg handling
//   - Restart count limits and restart_interval
//
// NOTE: This example demonstrates the intended API design. The runtime
// infrastructure (spawn, send, scheduler) is not yet functional.
//
// =============================================================================

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/behavior.hpp>
#include <hpactor/actor/message.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/supervision/one_for_one_supervisor.hpp>
#include <hpactor/supervision/supervision.hpp>
#include <iostream>
#include <variant>

// -----------------------------------------------------------------------------
// Supervision Concepts
// -----------------------------------------------------------------------------
//
// Supervision in actor systems provides fault-tolerance through hierarchical
// error handling:
//
//   Supervisor (parent)
//      |
//      +-- ChildActor1  (fails) --> Restart
//      +-- ChildActor2  (continues)
//      +-- ChildActor3  (continues)
//
// When a child actor fails:
//   1. The child sends a down_msg to its parent (supervisor)
//   2. The supervisor's strategy decides what to do
//   3. The strategy returns a SupervisionDirective
//
// SupervisionDirective:
//   - Restart: Restart the failed child
//   - Stop: Stop the failed child (and possibly others)
//   - Escalate: Pass the failure to this actor's supervisor
//
// SupervisionPolicy::Strategy:
//   - OneForOne: Only the failed child is affected
//   - AllForOne: All children are affected (restart all)
//
// -----------------------------------------------------------------------------

// -----------------------------------------------------------------------------
// WorkerActor - a simple actor that can fail
// -----------------------------------------------------------------------------

class WorkerActor : public hpactor::EventBasedActor {
  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[](hpactor::MessageVariant&& /*msg*/) {
            std::cout << "WorkerActor handling message" << std::endl;
        }};
    }

  public:
    WorkerActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys)
        : hpactor::EventBasedActor(ctx, sys) {}
};

// -----------------------------------------------------------------------------
// SupervisionPolicy Configuration
// -----------------------------------------------------------------------------

void demonstrate_supervision_policy() {
    std::cout << "=== SupervisionPolicy Configuration ===" << std::endl;

    // OneForOne - only the failed child is restarted
    // hpactor::SupervisionPolicy one_for_one{
    //     .strategy = hpactor::SupervisionPolicy::Strategy::OneForOne,
    //     .max_restarts = 3,                           // Max restarts within interval
    //     .restart_interval = std::chrono::seconds(5)  // Sliding window
    // };

    // AllForOne - all children are restarted when one fails
    // hpactor::SupervisionPolicy all_for_one{
    //     .strategy = hpactor::SupervisionPolicy::Strategy::AllForOne,
    //     .max_restarts = 3,
    //     .restart_interval = std::chrono::seconds(10)
    // };

    std::cout << "OneForOne: Only failed child is restarted" << std::endl;
    std::cout << "AllForOne: All children restart when one fails" << std::endl;
    std::cout << std::endl;
}

// -----------------------------------------------------------------------------
// Supervisor Implementation Pattern
// -----------------------------------------------------------------------------
//
// The SupervisorActor takes a Supervisor& strategy and a vector of children.
// It handles down_msg from children and delegates to the strategy.
//
// Pattern:
//   hpactor::OneForOneSupervisor strategy(policy);
//   std::vector<hpactor::Actor> children = { child1, child2 };
//   hpactor::SupervisorActor supervisor(ctx, sys, strategy, std::move(children));
//
// NOTE: The actual restart logic in SupervisorActor::restart_child is a stub
// (marked TODO). This example shows the intended API design.
//
// -----------------------------------------------------------------------------

class DatabaseSupervisor : public hpactor::SupervisorActor {
  public:
    DatabaseSupervisor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                       hpactor::SupervisionPolicy policy,
                       std::vector<hpactor::Actor> children)
        : hpactor::SupervisorActor(ctx, sys, strategy_, std::move(children)),
          strategy_(policy) {}

  private:
    hpactor::OneForOneSupervisor strategy_;
};

// -----------------------------------------------------------------------------
// SelfSupervisingActor Pattern
// -----------------------------------------------------------------------------
//
// SelfSupervisingActor manages its own children with a SupervisionPolicy.
// Override on_failure() to implement custom restart logic.
//
// Pattern:
//   class MyActor : public hpactor::SelfSupervisingActor {
//     protected:
//       SupervisionDirective on_failure(ActorId child_id, const error& err) override {
//         // Custom restart logic
//         return SupervisionDirective::Restart;
//       }
//   };
//
// -----------------------------------------------------------------------------

class SessionManager : public hpactor::SelfSupervisingActor {
  public:
    SessionManager(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                   hpactor::SupervisionPolicy policy)
        : hpactor::SelfSupervisingActor(ctx, sys, policy),
          policy_(policy) {}

  protected:
    // Override to implement custom restart behavior
    hpactor::SupervisionDirective on_failure(hpactor::ActorId child_id,
                                             const hpactor::error& err) override {
        std::cout << "SessionManager: child " << child_id.value()
                  << " failed with: " << err.message() << std::endl;

        // Custom logic: only restart if not too many failures
        return hpactor::SupervisionDirective::Restart;
    }

  private:
    hpactor::SupervisionPolicy policy_;
};

// -----------------------------------------------------------------------------
// Failure Handling Flow
// -----------------------------------------------------------------------------

void demonstrate_failure_flow() {
    std::cout << "\n=== Failure Handling Flow ===" << std::endl;

    std::cout << R"(
    1. Child actor encounters error (crash, exception, etc.)
           |
           v
    2. Child sends down_msg{address, error} to parent
           |
           v
    3. Supervisor::handle_child_down() called
           |
           v
    4. Supervisor::strategy_.on_child_failure(failure) called
           |                        |
           | OneForOne              | AllForOne
           v                        v
    5a. Restart child          5b. Restart ALL children
           |                        |
           v                        v
    6. If restart count exceeded within interval:
         - Stop child (and siblings for AllForOne)
         - Optionally escalate to OUR supervisor
    )" << std::endl;
}

// -----------------------------------------------------------------------------
// Main - demonstrates supervision usage
// -----------------------------------------------------------------------------

int main() {
    std::cout << "=== HPActor Example 04: Supervision Tree ===" << std::endl;

    demonstrate_supervision_policy();

    hpactor::Config config{
        .scheduler_threads = 4,
        .max_queue_depth = 1024
    };
    hpactor::ActorSystem system(config);

    demonstrate_failure_flow();

    std::cout << "\nNOTE: Actor spawning and message passing are not yet "
                 "fully implemented.\n"
              << "This example demonstrates the intended API usage patterns.\n"
              << std::endl;

    // -----------------------------------------------------------------
    // Pattern: Creating a supervised worker
    // -----------------------------------------------------------------
    // In a complete system:
    //
    //   // Create supervisor with OneForOne strategy
    //   hpactor::SupervisionPolicy policy{...};
    //   hpactor::OneForOneSupervisor strategy(policy);
    //   auto supervisor = system.spawn<DatabaseSupervisor>(policy, {});
    //
    //   // Supervisor manages workers
    //   auto worker1 = system.spawn<WorkerActor>();
    //   auto worker2 = system.spawn<WorkerActor>();
    //
    //   // Workers fail sometimes
    //   // When worker1 fails, supervisor restarts only worker1 (OneForOne)
    //   // worker2 continues unaffected
    //
    // -----------------------------------------------------------------
    // Pattern: Using SelfSupervisingActor
    // -----------------------------------------------------------------
    //   auto session_manager = system.spawn<SessionManager>(policy);
    //
    //   // Session manager adds children and supervises them
    //   // If one connection fails, all are restarted (AllForOne)

    std::cout << "Supervision patterns:" << std::endl;
    std::cout << "  1. SupervisorActor + Supervisor strategy (OneForOne/AllForOne)"
              << std::endl;
    std::cout << "  2. SelfSupervisingActor with custom on_failure()" << std::endl;
    std::cout << std::endl;
    std::cout << "Key API:" << std::endl;
    std::cout << "  SupervisionPolicy{strategy, max_restarts, restart_interval}"
              << std::endl;
    std::cout << "  OneForOneSupervisor(policy)" << std::endl;
    std::cout << "  SupervisorActor(ctx, sys, strategy, children)" << std::endl;
    std::cout << "  SelfSupervisingActor(ctx, sys, policy)" << std::endl;
    std::cout << "  on_failure(child_id, error) -> SupervisionDirective" << std::endl;

    return 0;
}
