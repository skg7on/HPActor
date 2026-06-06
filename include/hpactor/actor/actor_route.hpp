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

#include <hpactor/actor/lifecycle_state.hpp>
#include <hpactor/actor/passivation_config.hpp>
#include <hpactor/types/types.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hpactor {

class LocalActor;

/// \brief Abstraction over where messages are delivered.
///
/// Decouples delivery from actor liveness. The actor registry holds one
/// route per known actor. When an actor is active, a LocalActiveRoute
/// delegates directly to the mailbox. When passivated, a
/// LocalPassivatedRoute buffers messages and triggers lazy reactivation.
class IActorRoute {
  public:
    virtual ~IActorRoute() = default;

    /// \brief The lifecycle state of the target (or its stub).
    virtual LifecycleState state() const = 0;

    /// \brief Whether this route currently accepts user messages.
    virtual bool is_active() const = 0;

    /// \brief Human-readable description for CLI/debug.
    virtual std::string describe() const = 0;

    /// \brief The actor identity this route represents.
    virtual ActorId actor_id() const = 0;
};

/// \brief Route wrapping a live LocalActor.
class LocalActiveRoute : public IActorRoute {
  public:
    explicit LocalActiveRoute(LocalActor* actor);
    ~LocalActiveRoute() override;

    LifecycleState state() const override;
    bool is_active() const override;
    std::string describe() const override;
    ActorId actor_id() const override;

    LocalActor* actor() const noexcept {
        return actor_;
    }

  private:
    LocalActor* actor_;
};

/// \brief Route stub for a passivated actor.
///
/// Holds the actor identity, passivation metadata, and a bounded
/// reactivation buffer. The first message to arrive sets the atomic
/// reactivation flag and triggers recovery. Subsequent messages
/// enqueue to the buffer. Once reactivation succeeds, this stub is
/// replaced by a LocalActiveRoute.
class LocalPassivatedRoute : public IActorRoute {
  public:
    LocalPassivatedRoute(ActorId id, std::string persistence_id,
                         PassivationRecord record, uint32_t buffer_capacity = 64);
    ~LocalPassivatedRoute() override;

    LifecycleState state() const override;
    bool is_active() const override;
    std::string describe() const override;
    ActorId actor_id() const override;

    const std::string& persistence_id() const noexcept {
        return persistence_id_;
    }
    const PassivationRecord& record() const noexcept {
        return record_;
    }

    /// \brief Whether reactivation is currently in progress.
    bool reactivation_in_progress() const noexcept {
        return reactivation_in_progress_.load(std::memory_order_acquire);
    }

    /// \brief Claim the reactivation slot. Returns true if this call
    ///        won the CAS (caller must spawn reactivation).
    bool claim_reactivation();

    /// \brief Try to buffer a message during reactivation.
    ///
    /// Returns true if the buffer had room, false if it was full.
    bool try_buffer_message();

    /// \brief Number of messages currently buffered.
    uint32_t buffered_count() const;

    /// \brief Transition stored lifecycle state from Passivated to Recovering.
    void transition_to_recovering();

    /// \brief Transition stored lifecycle state to the given state.
    ///        Used by the reactivation task after recovery completes.
    void set_state(LifecycleState s);

  private:
    ActorId actor_id_;
    std::string persistence_id_;
    PassivationRecord record_;
    std::atomic<bool> reactivation_in_progress_{false};
    std::atomic<uint8_t> lifecycle_state_{
        static_cast<uint8_t>(LifecycleState::kPassivated)};
    uint32_t buffer_capacity_;
    mutable std::mutex buffer_mutex_;
    uint32_t buffer_count_{0};
};

} // namespace hpactor
