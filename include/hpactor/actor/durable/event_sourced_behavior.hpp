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

#include <hpactor/actor/durable/durable_behavior.hpp>
#include <hpactor/actor/durable/durable_state_store.hpp>
#include <hpactor/msg/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <string>
#include <utility>
#include <vector>

namespace hpactor::actor::durable {

/// \brief User-provided function to apply a domain event to state.
///
/// Must be specialized for each (State, Event) pair used with
/// \c EventSourcedBehavior.
template <typename State, typename Event>
result<void> apply_event_to_state(State& s, const Event& e);

/// \brief User-provided event serialization.
///
/// Must be specialized for each event type used with
/// \c EventSourcedBehavior.
template <typename Event> StreamBuffer serialize_event(const Event& e);

/// \brief User-provided event deserialization.
///
/// Must be specialized for each event type used with
/// \c EventSourcedBehavior.
template <typename Event>
result<Event> deserialize_event(const StreamBuffer& data);

/// \brief Event-sourced durable behavior with snapshot + event replay.
///
/// Extends the snapshot model of \c DurableBehavior with an append-only
/// event log. Recovery restores the latest snapshot and replays all
/// events with sequence > snapshot sequence. New events are persisted
/// to the store before being applied to state.
///
/// \tparam State  The actor's in-memory state type (default-constructible,
///                moveable). Must provide \c serialize_state and
///                \c deserialize_state specializations.
/// \tparam Event  Domain event type. Must provide \c serialize_event,
///                \c deserialize_event, and \c apply_event_to_state
///                specializations.
///
/// Usage:
/// \code
/// struct State { int count = 0; };
/// struct Evt { int delta = 0; };
/// template <> result<void> apply_event_to_state<State,Evt>(State&, const
/// Evt&); template <> StreamBuffer serialize_event<Evt>(const Evt&); template
/// <> result<Evt> deserialize_event<Evt>(const StreamBuffer&);
///
/// EventSourcedBehavior<State, Evt> behavior("actor-1", store, State{});
/// behavior.recover();
/// behavior.persist_event(Evt{5});
/// \endcode
template <typename State, typename Event> class EventSourcedBehavior {
  public:
    /// \brief Construct with persistence identity, store reference, and
    ///        initial state.
    ///
    /// The actor is not recovered until \c recover() is called.
    EventSourcedBehavior(std::string persistence_id, DurableStateStore& store,
                         State initial)
        : persistence_id_(std::move(persistence_id)), store_(store),
          state_(std::move(initial)) {}

    /// \brief Restore state from the latest snapshot, then replay all
    ///        subsequent events.
    ///
    /// If no snapshot exists, the initial state is used. If event
    /// deserialization or application fails, an error is returned.
    ///
    /// \return \c result<void>::make() on success.
    result<void> recover() {
        auto snap_result = store_.load_latest_snapshot(persistence_id_);
        if (snap_result.ok()) {
            auto& snap = snap_result.value();
            auto state_result = deserialize_state<State>(snap.data);
            if (!state_result.ok())
                return result<void>::make(error(
                    static_cast<uint32_t>(FailureReason::ReactivationFailed)));
            state_ = std::move(state_result.value());
            last_snapshot_sequence_ = snap.sequence;

            auto events_result =
                store_.load_events_after(persistence_id_, last_snapshot_sequence_);
            if (events_result.ok()) {
                for (const auto& ev_rec : events_result.value()) {
                    auto ev = deserialize_event<Event>(ev_rec.event_data);
                    if (!ev.ok())
                        continue;
                    auto apply_result = apply_event_to_state(state_, ev.value());
                    if (!apply_result.ok())
                        return apply_result;
                    last_event_sequence_ = ev_rec.sequence;
                }
            }
        }
        recovered_ = true;
        return result<void>::make();
    }

    /// \brief Persist an event to the store and apply it to state.
    ///
    /// The event is serialized and appended to the store with the next
    /// sequence number. If the store write succeeds, the event is applied
    /// to the in-memory state.
    ///
    /// \return \c result<void>::make() on success.
    result<void> persist_event(const Event& event) {
        auto data = serialize_event(event);
        uint64_t next_seq = last_event_sequence_ + 1;
        auto append_result =
            store_.append_event(persistence_id_, next_seq, std::move(data));
        if (!append_result.ok())
            return append_result;
        last_event_sequence_ = next_seq;
        return apply_event_to_state(state_, event);
    }

    /// \brief Persist an event and apply it (alias for \c persist_event).
    result<void> persist_event_and_apply(const Event& event) {
        return persist_event(event);
    }

    /// \brief Persist the current state as a snapshot.
    ///
    /// Events with sequence > this snapshot's sequence will be replayed
    /// on next recovery. Earlier events are covered by the snapshot.
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

    /// \brief Whether \c recover() has completed successfully.
    bool is_recovered() const {
        return recovered_;
    }

  private:
    std::string persistence_id_;
    DurableStateStore& store_;
    State state_;
    uint64_t last_snapshot_sequence_ = 0;
    uint64_t last_event_sequence_ = 0;
    bool recovered_ = false;
};

} // namespace hpactor::actor::durable
