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

#pragma once

#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/routing/routing_logic.hpp>
#include <hpactor/config/actor_factory.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/supervision/supervision.hpp>

#include <memory>
#include <vector>

namespace hpactor::routing {

/// \brief Pool Router — creates a pool of child actors and routes messages
///        to them using a pluggable routing strategy.
///
/// \c PoolRouter extends \c SelfSupervisingActor to inherit child lifecycle
/// management, restart counting, failure escalation, and quarantine.
/// Routees are spawned as children during \c on_activate().
///
/// Usage:
/// \code{.cpp}
/// auto router = system.spawn<PoolRouter>(
///     std::make_unique<RoundRobinLogic>(),
///     [](ActorContext* ctx, ActorSystem& sys) {
///         return std::make_shared<MyWorkerActor>(ctx, sys);
///     },
///     5  // pool size
/// );
/// context()->send(router.address(), TypedMessage(tag, payload));
/// \endcode
class PoolRouter final : public SelfSupervisingActor {
  public:
    /// \brief Construct a pool router.
    ///
    /// \param[in] ctx Actor context (nullptr during spawn, set by ActorSystem).
    /// \param[in] sys Actor system reference.
    /// \param[in] logic The routing strategy (ownership taken).
    /// \param[in] factory Factory function to create routee children.
    /// \param[in] pool_size Initial number of routees to spawn.
    /// \param[in] policy Supervision policy for child routees.
    PoolRouter(ActorContext* ctx, ActorSystem& sys,
               std::unique_ptr<IRoutingLogic> logic, config::ActorFactory factory,
               size_t pool_size, SupervisionPolicy policy = SupervisionPolicy{});

    /// \brief Spawn initial routee children. Called by ActorSystem after
    ///        context is set.
    void on_activate() override;

    /// \brief Intercept all incoming messages and forward to a selected
    ///        routee.
    Behavior make_behavior() override;

    // ── Routee management ─────────────────────────────────────────

    /// \brief Add one routee and spawn it as a child.
    void add_routee();

    /// \brief Remove the last routee from the pool.
    void remove_routee();

    /// \brief Resize the pool to \p new_size routees.
    ///
    /// Scales up by spawning new routees, or scales down by removing
    /// the last N routees.
    /// \param[in] new_size Target number of routees.
    void resize(size_t new_size);

    /// \brief Current number of routees in the pool.
    [[nodiscard]] size_t routee_count() const {
        return routees_.size();
    }

    /// \brief Access the routee \c ActorRef list (non-const for mailbox
    ///        snapshot collection).
    [[nodiscard]] std::vector<ActorRef>& routees() {
        return routees_;
    }
    [[nodiscard]] const std::vector<ActorRef>& routees() const {
        return routees_;
    }

    // ── Broadcast ──────────────────────────────────────────────────

    /// \brief Send a copy of \p msg to every routee in the pool.
    void broadcast(TypedMessage msg);

    // ── Routing logic ──────────────────────────────────────────────

    /// \brief Replace the current routing logic at runtime.
    /// \param[in] logic New routing strategy (ownership taken).
    void set_routing_logic(std::unique_ptr<IRoutingLogic> logic);

    /// \brief Access the current routing logic.
    [[nodiscard]] IRoutingLogic* routing_logic() const {
        return routing_logic_.get();
    }

    // ── Supervision ────────────────────────────────────────────────

    /// \brief Handle child failure — replace the failed routee.
    ///
    /// On \c Restart: the failed routee is replaced by spawning a new
    /// child from the factory.
    /// On \c Stop: the failed routee is removed from the pool.
    /// On \c Quarantine: the routee is left in the pool but its state
    /// is \c kQuarantined.
    SupervisionDirective on_failure(ActorId child_id, const error& err) override;

  private:
    /// \brief Spawn \p count routees and add them as children.
    void spawn_routees(size_t count);

    /// \brief Collect mailbox snapshots from all routees.
    void snapshot_routees(std::vector<cli::MboxSnapshot>& out);

    std::unique_ptr<IRoutingLogic> routing_logic_;
    config::ActorFactory factory_;
    size_t pool_size_{0};
    std::vector<ActorRef> routees_;
};

} // namespace hpactor::routing
