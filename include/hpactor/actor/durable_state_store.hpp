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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/types/types.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor {

/// \brief A persisted snapshot record produced by \c
/// DurableStateStore::write_snapshot().
///
/// Carries the serialized actor state plus metadata for schema evolution,
/// integrity verification, and recovery ordering.
struct SnapshotRecord {
    /// \brief Stable actor identity. Used as the store-level key across
    ///        passivation and restart cycles.
    std::string persistence_id;

    /// \brief Monotonic sequence number assigned by the store on write.
    ///
    /// Incremented per \c persistence_id. Recovery replays events with
    /// sequence > this value.
    uint64_t sequence = 0;

    /// \brief Actor schema version at the time the snapshot was taken.
    ///
    /// Compared against \c PassivationConfig::schema_version during
    /// recovery. A mismatch triggers \c IDurableActor::migrate_snapshot().
    uint32_t schema_version = 1;

    /// \brief Reserved for future serializer selection.
    uint32_t serializer_id = 0;

    /// \brief Monotonic timestamp captured by the store at write time
    ///        (milliseconds since an unspecified epoch).
    uint64_t timestamp_ms = 0;

    /// \brief Serialized actor state produced by
    ///        \c IDurableActor::snapshot_state().
    StreamBuffer data;

    /// \brief CRC32C checksum of \c data. Computed by the store on write
    ///        and verified on read.
    uint32_t checksum = 0;
};

/// \brief A persisted event record (for event-sourced actors).
///
/// Appended by \c DurableStateStore::append_event() and replayed during
/// recovery via \c IDurableActor::apply_event().
struct EventRecord {
    /// \brief Stable actor identity.
    std::string persistence_id;

    /// \brief Monotonic sequence number assigned by the store on append.
    uint64_t sequence = 0;

    /// \brief Actor schema version when the event was emitted.
    uint32_t schema_version = 1;

    /// \brief Reserved for future serializer selection.
    uint32_t serializer_id = 0;

    /// \brief Monotonic timestamp captured by the store at append time
    ///        (milliseconds since an unspecified epoch).
    uint64_t timestamp_ms = 0;

    /// \brief Serialized event payload. Passed to
    ///        \c IDurableActor::apply_event() during replay.
    StreamBuffer event_data;
};

/// \brief Abstract persistence backend for durable actor state.
///
/// One store instance serves all durable actors on a node. The store owns
/// sequence numbering: each \c write_snapshot() and \c append_event()
/// assigns the next monotonic sequence for the given \c persistence_id.
///
/// Implementations:
/// - \c InMemoryStateStore — \c std::unordered_map backend, for tests.
/// - \c FileStateStore — one file per actor, atomic rename on write,
///   CRC32C integrity verification.
///
/// \note Thread safety: All public methods are safe to call concurrently
///       from any thread. Implementations must provide internal
///       synchronization.
class DurableStateStore {
  public:
    virtual ~DurableStateStore() = default;

    /// \brief Persist a snapshot and return the assigned record.
    ///
    /// The store assigns the next monotonic sequence number and timestamp
    /// for \p persistence_id.
    ///
    /// \param[in] persistence_id Stable actor identity. Must match the
    ///                           value returned by
    ///                           \c IDurableActor::persistence_id().
    /// \param[in] schema_version Actor's current schema version from
    ///                           \c PassivationConfig::schema_version.
    /// \param[in] data           Serialized actor state from
    ///                           \c IDurableActor::snapshot_state().
    ///                           Ownership transfers to the store.
    /// \return The persisted record with assigned sequence and timestamp,
    ///         or an error (typically \c PassivationSnapshotFailed).
    virtual result<SnapshotRecord>
    write_snapshot(std::string_view persistence_id, uint32_t schema_version,
                   StreamBuffer data) = 0;

    /// \brief Load the most recent snapshot for an actor.
    ///
    /// \param[in] persistence_id Stable actor identity.
    /// \return The latest snapshot record, or an error if no snapshot
    ///         exists for this \p persistence_id.
    virtual result<SnapshotRecord>
    load_latest_snapshot(std::string_view persistence_id) = 0;

    /// \brief Append an event for an event-sourced actor.
    ///
    /// The store assigns the next monotonic sequence number. Duplicate
    /// sequences are silently accepted (idempotent). Gaps in the sequence
    /// produce an error.
    ///
    /// \param[in] persistence_id Stable actor identity.
    /// \param[in] sequence       Expected next sequence number. The store
    ///                           validates this is either the next expected
    ///                           value (appends) or a value already seen
    ///                           (idempotent no-op).
    /// \param[in] event          Serialized event data. Ownership
    ///                           transfers to the store.
    /// \return \c result<void>::make() on success, or an error if the
    ///         sequence has a gap.
    virtual result<void> append_event(std::string_view persistence_id,
                                      uint64_t sequence, StreamBuffer event) = 0;

    /// \brief Load events with sequence numbers greater than
    ///        \p after_sequence.
    ///
    /// Used during recovery to replay events that occurred after the
    /// restored snapshot.
    ///
    /// \param[in] persistence_id  Stable actor identity.
    /// \param[in] after_sequence  Return only events with
    ///                            \c sequence > \p after_sequence.
    /// \return Ordered vector of event records (ascending sequence),
    ///         or an empty vector if no events exist after
    ///         \p after_sequence.
    virtual result<std::vector<EventRecord>>
    load_events_after(std::string_view persistence_id, uint64_t after_sequence) = 0;

    /// \brief Delete all state for an actor.
    ///
    /// Called during shutdown of a passivated durable actor, or when an
    /// operator explicitly purges state. Irreversible — the caller must
    /// ensure the actor will not be reactivated.
    ///
    /// \param[in] persistence_id Stable actor identity.
    /// \return \c result<void>::make() on success, or an error if the
    ///         underlying storage operation fails.
    virtual result<void> delete_state(std::string_view persistence_id) = 0;

    /// \brief Human-readable store type name for CLI introspection and
    ///        diagnostics.
    ///
    /// \return A \c std::string_view identifying the store implementation
    ///         (e.g. \c "in_memory", \c "file").
    virtual std::string_view store_type() const = 0;
};

} // namespace hpactor
