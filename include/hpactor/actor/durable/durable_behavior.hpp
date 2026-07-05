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

#include <hpactor/actor/durable/durable_state_store.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <string>
#include <utility>

namespace hpactor::actor::durable {

/// \brief User-provided serialization of state \c State to bytes.
///
/// Must be specialized for each concrete state type used with
/// \c DurableBehavior.
template <typename State> StreamBuffer serialize_state(const State& s);

/// \brief User-provided deserialization of state \c State from bytes.
///
/// Must be specialized for each concrete state type used with
/// \c DurableBehavior.
template <typename State>
result<State> deserialize_state(const StreamBuffer& data);

/// \brief Snapshot-based durable behavior — no event sourcing.
///
/// Persists actor state via periodic snapshots. Recovery restores the
/// latest snapshot. Use \c EventSourcedBehavior for append-only event
/// sourcing with replay.
///
/// \tparam State The actor's in-memory state type. Must be
///               default-constructible and moveable. The user must
///               specialize \c serialize_state and \c deserialize_state.
///
/// Usage:
/// \code
/// struct OrderState { int count = 0; };
/// template <> StreamBuffer serialize_state<OrderState>(const OrderState& s);
/// template <> result<OrderState> deserialize_state<OrderState>(const
/// StreamBuffer&);
///
/// DurableBehavior<OrderState> behavior("order-42", store, OrderState{});
/// behavior.recover();
/// behavior.state().count += 1;
/// behavior.snapshot();
/// \endcode
template <typename State> class DurableBehavior {
  public:
    /// \brief Construct with a persistence identity, store reference, and
    ///        initial state.
    ///
    /// The actor is not recovered until \c recover() is called. Until then,
    /// \c is_recovered() returns false and \c state() returns the initial
    /// state.
    DurableBehavior(std::string persistence_id, DurableStateStore& store,
                    State initial)
        : persistence_id_(std::move(persistence_id)), store_(store),
          state_(std::move(initial)) {}

    /// \brief Restore state from the latest snapshot in the store.
    ///
    /// If no snapshot exists, the initial state provided at construction
    /// is preserved and recovery succeeds. If deserialization fails,
    /// \c FailureReason::ReactivationFailed is returned.
    ///
    /// \return \c result<void>::make() on success.
    result<void> recover() {
        auto snap_result = store_.load_latest_snapshot(persistence_id_);
        if (snap_result.ok()) {
            auto& snap = snap_result.value();
            auto state_result = deserialize_state<State>(snap.data);
            if (!state_result.ok()) {
                return result<void>::make(error(
                    static_cast<uint32_t>(FailureReason::ReactivationFailed)));
            }
            state_ = std::move(state_result.value());
            last_snapshot_sequence_ = snap.sequence;
        }
        recovered_ = true;
        return result<void>::make();
    }

    /// \brief Persist the current state as a snapshot.
    ///
    /// Serializes state via \c serialize_state and writes it to the store
    /// with the current schema version (1 by default).
    ///
    /// \return \c result<void>::make() on success, or
    ///         \c FailureReason::PassivationSnapshotFailed on store failure.
    result<void> snapshot() {
        auto data = serialize_state(state_);
        auto write_result =
            store_.write_snapshot(persistence_id_, 1, std::move(data));
        if (write_result.ok()) {
            last_snapshot_sequence_ = write_result.value().sequence;
            return result<void>::make();
        }
        return result<void>::make(error(
            static_cast<uint32_t>(FailureReason::PassivationSnapshotFailed)));
    }

    /// \brief Mutable access to the actor's state.
    State& state() {
        return state_;
    }
    /// \brief Immutable access to the actor's state.
    const State& state() const {
        return state_;
    }

    /// \brief The stable persistence identity for this actor.
    const std::string& persistence_id() const {
        return persistence_id_;
    }

    /// \brief Whether \c recover() has completed successfully.
    bool is_recovered() const {
        return recovered_;
    }

  private:
    std::string persistence_id_;
    DurableStateStore& store_;
    State state_;
    uint64_t last_snapshot_sequence_ = 0;
    bool recovered_ = false;
};

} // namespace hpactor::actor::durable
