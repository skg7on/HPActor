# Durable Actor State Architecture Design

## 1. Executive Summary

Many actors can remain ephemeral, but production systems also need actors whose
state survives process and node failure. HPActor should support durable actor
state as an opt-in model with snapshots, event sourcing, passivation, recovery,
and storage adapters.

This design separates actor execution from persistence. Event-based actors keep
their turn-based processing model, while durable actors use explicit persistence
APIs and recovery hooks.

## 2. Goals

1. Persist selected actor state across restarts.
2. Support both snapshot and event-sourced models.
3. Enable passivation and later reactivation for idle actors.
4. Integrate with sharding and placement.
5. Provide schema evolution and recovery diagnostics.
6. Keep non-durable actors lightweight.

## 3. Non-Goals

- Transparent persistence for arbitrary C++ object graphs.
- Distributed transactions across multiple actors.
- Exactly-once side effects outside HPActor.

## 4. Durable Actor Modes

### 4.1 Snapshot Mode

Actor periodically writes full state snapshots.

Best for:

- Compact state.
- Low update frequency.
- Fast recovery.

### 4.2 Event-Sourced Mode

Actor persists domain events before applying them to memory state.

Best for:

- Auditable state changes.
- High recovery fidelity.
- Replay and temporal debugging.

### 4.3 Hybrid Mode

Actor writes events and periodic snapshots. Recovery loads latest snapshot then
replays later events.

## 5. Actor Interfaces

```cpp
class DurableActor {
  public:
    virtual std::string persistence_id() const = 0;
    virtual result<Bytes> snapshot_state() const = 0;
    virtual result<void> restore_snapshot(const Bytes& data) = 0;
    virtual result<void> apply_event(const Bytes& event) = 0;
};
```

Event-sourced actors additionally expose:

```cpp
result<void> persist_event(Bytes event);
result<void> persist_event_and_apply(Bytes event);
```

## 6. Storage Adapter

```cpp
class DurableStateStore {
  public:
    virtual result<void> write_snapshot(const SnapshotRecord& record) = 0;
    virtual result<SnapshotRecord> load_latest_snapshot(
        std::string_view persistence_id) = 0;

    virtual result<void> append_event(const EventRecord& record) = 0;
    virtual result<std::vector<EventRecord>> load_events_after(
        std::string_view persistence_id,
        uint64_t sequence) = 0;
};
```

Initial adapters:

- `InMemoryStateStore` for tests.
- `FileStateStore` for local durability.
- Future adapters for RocksDB, PostgreSQL, or object storage.

## 7. Recovery Lifecycle

1. Actor is spawned or shard activates.
2. Runtime detects durable config.
3. Store loads latest snapshot.
4. Actor restores snapshot.
5. Store loads events after snapshot sequence.
6. Actor replays events in order.
7. Actor becomes ready and starts accepting user messages.

Recovery failure policy:

- `FailActor`: actor fails and supervisor handles restart.
- `QuarantineActor`: actor exists but rejects user messages.
- `SkipCorruptEvent`: only for explicitly configured tolerant actors.

## 8. Passivation

Passivation is an explicit lifecycle transition:

1. Stop accepting new user messages.
2. Drain or reroute mailbox based on policy.
3. Persist final snapshot if configured.
4. Release actor memory.
5. Keep logical actor route active through shard owner.

Reactivation reloads state from storage before processing new messages.

## 9. Schema Evolution

Durable records include:

- persistence id
- actor type
- schema version
- serializer id
- sequence number
- timestamp
- checksum

Migration strategies:

- In-place snapshot upgrade during recovery.
- Event upcaster chain.
- Reject unsupported schema with clear diagnostics.

## 10. Observability

Metrics:

- `hpactor_durable_recovery_total`
- `hpactor_durable_recovery_duration_seconds`
- `hpactor_durable_snapshot_write_total`
- `hpactor_durable_event_append_total`
- `hpactor_durable_store_errors_total`

CLI:

- `/durable actor <id> state`
- `/durable actor <id> recover`
- `/durable store stats`

## 11. Acceptance Criteria

- Durable actors opt in explicitly.
- Non-durable actors do not pay persistence overhead.
- Snapshot recovery works before user messages are handled.
- Event replay preserves event order per actor.
- Store errors produce structured actor failure or quarantine behavior.
- Passivation can release actor memory without losing durable state.

