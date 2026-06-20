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
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/receptionist/service_key.hpp>
#include <hpactor/actor/routing/routing_logic.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/ref/actor_ref.hpp>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor::routing {

/// \brief Group Router — routes messages to a dynamic set of externally-
///        registered actors using a pluggable routing strategy.
///
/// \c GroupRouter extends \c EventBasedActor. Routees are NOT children —
/// they are discovered or registered actors that exist independently.
/// No supervision is applied to routees.
///
/// Usage:
/// \code{.cpp}
/// auto router = system.spawn<GroupRouter>(
///     std::make_unique<ConsistentHashingLogic>(),
///     "image-processor"  // service key
/// );
/// router->add_routee(ActorRef(some_actor));
/// context()->send(router.address(), TypedMessage(tag, payload));
/// \endcode
class GroupRouter final : public EventBasedActor {
  public:
    /// \brief Construct a group router.
    ///
    /// \param[in] ctx Actor context (nullptr during spawn, set by ActorSystem).
    /// \param[in] sys Actor system reference.
    /// \param[in] logic The routing strategy (ownership taken).
    /// \param[in] service_key String key for service discovery /
    /// identification.
    GroupRouter(ActorContext* ctx, ActorSystem& sys,
                std::unique_ptr<IRoutingLogic> logic, std::string service_key);

    /// Construct a GroupRouter that discovers routees via the Receptionist.
    GroupRouter(ActorContext* ctx, ActorSystem& sys,
                receptionist::ServiceKey service_key,
                std::unique_ptr<IRoutingLogic> logic);

    /// \brief Intercept all incoming messages and forward to a selected
    ///        routee.
    Behavior make_behavior() override;

    // ── Routee management ─────────────────────────────────────────

    /// \brief Add a routee to the group.
    /// \param[in] routee Actor reference to add.
    void add_routee(ActorRef routee);

    /// \brief Remove a routee by address.
    /// \param[in] addr Address of the routee to remove.
    void remove_routee(const ActorAddress& addr);

    /// \brief Replace the entire routee set.
    /// \param[in] routees New list of routee references.
    void set_routees(std::vector<ActorRef> routees);

    /// \brief Current number of routees in the group.
    [[nodiscard]] size_t routee_count() const {
        return routees_.size();
    }

    /// \brief Access the routee \c ActorRef list (non-const for snapshot
    ///        collection).
    [[nodiscard]] std::vector<ActorRef>& routees() {
        return routees_;
    }
    [[nodiscard]] const std::vector<ActorRef>& routees() const {
        return routees_;
    }

    // ── Service key ───────────────────────────────────────────────

    /// \brief Service key for discovery.
    [[nodiscard]] const std::string& service_key() const {
        return service_key_;
    }

    // ── Broadcast ─────────────────────────────────────────────────

    /// \brief Send a copy of \p msg to every routee in the group.
    void broadcast(TypedMessage msg);

    // ── Routing logic ─────────────────────────────────────────────

    /// \brief Replace the current routing logic at runtime.
    /// \param[in] logic New routing strategy (ownership taken).
    void set_routing_logic(std::unique_ptr<IRoutingLogic> logic);

    /// \brief Access the current routing logic.
    [[nodiscard]] IRoutingLogic* routing_logic() const {
        return routing_logic_.get();
    }

  private:
    std::unique_ptr<IRoutingLogic> routing_logic_;
    std::string service_key_;
    std::optional<receptionist::ServiceKey> receptionist_key_;
    bool needs_snapshots_{false};
    std::vector<ActorRef> routees_;
};

} // namespace hpactor::routing
