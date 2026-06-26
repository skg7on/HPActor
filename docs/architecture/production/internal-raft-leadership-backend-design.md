# Internal Raft Leadership Backend Design

## 1. Executive Summary

The production distributed leadership design uses an external coordinator first.
This document defines the future internal Raft backend that can provide the same
`ILeadershipBackend` contract without changing singleton or shard-coordinator
consumers.

The Raft backend stores cluster leadership metadata in a replicated log. A
singleton lease is valid only after a leadership grant or renewal entry is
committed by a quorum of the current Raft voter set. The fencing token is derived
from the committed `(term, log_index)` pair, which gives every mutating
cluster-control action a globally ordered authority marker.

Gossip and `ClusterFailureModel` remain useful detection and eligibility inputs,
but Raft owns the production decision about who may mutate singleton and shard
ownership state.

## 2. Goals

1. Implement the same `ILeadershipBackend` contract as etcd and Consul adapters.
2. Provide exactly-one committed owner for each production cluster singleton.
3. Generate monotonic fencing tokens from committed Raft entries.
4. Use committed voter configurations for quorum decisions.
5. Support safe membership changes through joint consensus.
6. Persist Raft term, vote, log, snapshots, and applied index across restart.
7. Keep Raft network and disk work out of actor scheduler hot paths.
8. Provide deterministic unit, model, partition, and crash-recovery tests.

## 3. Non-Goals

- Implementing this backend in the first external-coordinator milestone.
- Using Raft for ordinary actor message delivery.
- Replacing SWIM gossip service discovery.
- Supporting Byzantine fault tolerance.
- Relying on exception-based control flow or RTTI.

## 4. Position In Architecture

```text
ClusterLeadershipManagerActor
        |
        v
ILeadershipBackend
        |
        v
RaftLeadershipBackend
        |
        +-- RaftNodeCore
        +-- RaftLogStore
        +-- RaftTransport
        +-- RaftSnapshotStore
        +-- LeadershipStateMachine
```

`RaftLeadershipBackend` adapts Raft commits into `LeadershipLease` results. The
rest of the cluster singleton stack sees the same methods it uses for external
coordinators: `try_acquire()`, `renew()`, `release()`, `current_owner()`, and
`watch()`.

## 5. Core Components

| Component | Purpose |
|-----------|---------|
| `RaftLeadershipBackend` | Implements `ILeadershipBackend` by proposing leadership commands to Raft and projecting committed entries into leases. |
| `RaftNodeCore` | Deterministic Raft state machine: follower, candidate, leader, term, vote, log matching, commit index. |
| `RaftLogStore` | Durable append-only log with term/index metadata, CRC, and atomic segment replacement. |
| `RaftSnapshotStore` | Durable snapshots of applied leadership state and voter configuration. |
| `RaftTransport` | Bounded control-plane RPC transport for AppendEntries, RequestVote, PreVote, InstallSnapshot, and responses. |
| `LeadershipStateMachine` | Applies committed log entries to singleton ownership records. |
| `RaftClock` | Injected logical/monotonic clock for deterministic election and renewal tests. |

The core classes are testable without `ActorSystem`. Actor wrappers add message
dispatch, lifecycle integration, metrics, CLI snapshots, and transport wiring.

### Backend API Mapping

| Backend Method | Raft Action | Return Rule |
|----------------|-------------|-------------|
| `try_acquire(singleton, identity)` | Propose `GrantLeadership` if the applied state allows the identity to own the singleton. | Return `LeadershipLease` only after the grant entry is committed and applied locally. |
| `renew(singleton, identity, token)` | Propose `RenewLeadership` referencing the current fencing token. | Return a lease with a higher token after the renewal entry commits. |
| `release(singleton, identity, token)` | Propose `ReleaseLeadership` for the current owner token. | Return success after the release entry commits; stale releases are idempotent no-ops. |
| `current_owner(singleton)` | Read from applied leadership state. | For linearizable reads, route through the Raft leader or perform a read-index check. |
| `watch(singleton)` | Subscribe to applied ownership transitions. | Emit owner, token, loss, and unavailable events from committed state only. |

The adapter must not report local candidate, leader, or gossip state as a
leadership lease. Only committed and applied Raft state can create backend
ownership.

## 6. Persistent State

Each Raft node persists:

- `current_term`
- `voted_for`
- log entries with `(index, term, command, crc)`
- committed voter configuration
- latest snapshot metadata
- `last_applied`
- node identity used for the persisted vote

Writes that update term, vote, or log entries must be durable before the node
sends a response that depends on them. The storage layer should expose explicit
result codes for I/O failure and corruption; it must not depend on exceptions.

## 7. Volatile State

Each Raft node tracks:

- role: `Follower`, `PreCandidate`, `Candidate`, `Leader`
- `commit_index`
- `last_applied`
- leader id, if known
- election timeout deadline
- heartbeat deadline
- per-peer `next_index` and `match_index` when leader
- recent quorum contact for check-quorum behavior

Election timers are randomized within a configured range. Tests inject the clock
and deterministic timer values rather than sleeping on wall time.

## 8. Raft Commands

The replicated leadership state machine uses explicit command entries:

```text
GrantLeadership(singleton, owner_identity, membership_epoch, ttl_ms)
RenewLeadership(singleton, owner_identity, previous_token, ttl_ms)
ReleaseLeadership(singleton, owner_identity, fencing_token)
StepDown(singleton, owner_identity, fencing_token, reason)
ChangeVotersBegin(old_voters, new_voters)
ChangeVotersCommit(new_voters)
SnapshotMarker(last_included_index, last_included_term)
```

Rules:

- `GrantLeadership` succeeds only if no live owner exists, the previous owner
  has released or stepped down, the configured regrant deadline has passed, or
  the command is an idempotent refresh from the current owner.
- `RenewLeadership` must reference the current fencing token.
- `ReleaseLeadership` and `StepDown` are accepted only when they reference the
  currently applied owner token. Administrative force-stepdown should be a
  separate audited command if the implementation needs it later.
- voter changes cannot be interleaved with unsafe single-step membership
  replacement.

## 9. Fencing Token

The backend generates a fencing token from the committed grant or renewal entry:

```text
fencing_token = (term << 32) | log_index
```

The exact packing can change if the implementation needs more index bits, but
the ordering rule cannot: a token from a later committed Raft term/index pair is
greater than the previous token for the same singleton.

Consumers compare only the token. Diagnostics preserve term and index as
separate fields in `LeadershipLease`.

## 10. Leadership Lease Semantics

Raft does not rely on local wall-clock leases for safety. Lease validity is a
cluster-control admission rule derived from committed entries and renewal
deadlines.

Safety rules:

- A node becomes singleton owner only after it observes a committed
  `GrantLeadership` or `RenewLeadership` entry for its identity.
- A leader that loses Raft quorum must stop renewing singleton leases and step
  down from active singleton ownership.
- A local node may accept mutating singleton work only while the latest applied
  lease belongs to its node identity and the local safety deadline has not
  passed.
- A stale owner receiving delayed work with an older token rejects it with
  `FencingTokenStale`.
- TTLs are fail-closed admission deadlines and regrant inputs, not the only
  protection for downstream state. Mutating cluster-control writes still carry
  and validate the latest fencing token.

This keeps the same fail-closed contract as the external coordinator backend.

## 11. Election Protocol

The Raft backend uses:

- PreVote before incrementing term, to reduce disruption from isolated nodes.
- RequestVote with standard last-log-term and last-log-index checks.
- AppendEntries heartbeats from the leader.
- CheckQuorum so an isolated leader steps down.
- Leader transfer as an optional operator action for graceful maintenance.

Election must never depend on `GossipMembership::alive_nodes()`. Gossip can
influence whether a node campaigns, but the Raft voter set and log freshness
rules decide whether a vote is granted.

## 12. Log Replication

AppendEntries carries:

- leader term,
- leader id,
- previous log index and term,
- entries,
- leader commit index,
- committed voter configuration id.

Followers reject mismatched previous entries. Leaders decrement `next_index` or
use conflict hints until replication catches up. A log entry is committed when it
is replicated to a majority of the active voter configuration and belongs to the
current leader term, following the usual Raft commitment rule.

## 13. Membership Changes

Membership changes use joint consensus:

1. Commit `ChangeVotersBegin(old_voters, new_voters)`.
2. During joint consensus, commits require quorum from both old and new voter
   sets.
3. Catch up new voters as learners before promoting them.
4. Commit `ChangeVotersCommit(new_voters)`.
5. After the commit is applied, the new voter set becomes the only quorum set.

The backend must not add or remove voters solely from a local failure-detector
view. Operator action, config reconciliation, or a higher-level cluster-control
workflow proposes voter changes.

## 14. Snapshots And Compaction

Snapshots include:

- singleton ownership records,
- latest fencing token per singleton,
- committed voter configuration,
- last included index and term,
- membership epoch seen by the leadership state machine.

Compaction may remove log entries up to the snapshot index only after the
snapshot is durable. A follower that is too far behind receives
`InstallSnapshot`. After installing a snapshot, the follower must preserve token
ordering and reject any stale log entry before the snapshot index.

## 15. Runtime Integration

### Actor Boundary

Raft actor wrappers own message dispatch and lifecycle, but `RaftNodeCore` owns
the deterministic protocol state. Disk writes and network sends are delegated to
bounded runtime services.

### Blocking Work

Durable log fsync, snapshot writes, DNS resolution, and blocking transport calls
must not run on cooperative scheduler hot paths. Use a blocking executor,
daemon actor, or async transport path.

### Backpressure

Raft transport queues are bounded. When a peer is slow:

- AppendEntries batching is limited by bytes and entry count.
- Snapshot sends are chunked.
- The leader retains only configured in-memory replication buffers.
- Slow peers catch up from the log store or snapshots.

### Configuration

```toml
[system.cluster.leadership]
backend = "raft"

[system.cluster.leadership.raft]
group_id = "cluster-control"
node_id = "node-a"
voters = ["node-a", "node-b", "node-c"]
data_dir = "var/hpactor/raft"
election_timeout_min_ms = 150
election_timeout_max_ms = 300
heartbeat_interval_ms = 50
snapshot_entries = 10000
max_append_entries_bytes = 65536
check_quorum = true
pre_vote = true
```

The configured voter set is a bootstrap input. After the first committed voter
configuration exists, the persisted Raft configuration is authoritative and
runtime membership changes must flow through joint consensus.

## 16. Failure Semantics

| Condition | Action | Failure |
|-----------|--------|---------|
| No Raft quorum | Step down from production singleton ownership. | `LeadershipUnavailable` |
| Higher term observed | Become follower and stop active mutations. | `LeadershipLost` |
| Log corruption | Quarantine local Raft backend until recovery. | `StorageCorruption` |
| Stale token command | Reject command. | `FencingTokenStale` |
| Voter config mismatch | Reject append/vote and request sync. | `StaleMembershipEpoch` |
| Snapshot install failure | Keep follower unavailable for leadership. | `LeadershipUnavailable` |

Failures that reject user messages should produce `FailureEnvelope` and DLQ
records where the data plane already requires them.

## 17. Observability

Metrics:

- `hpactor_raft_role`
- `hpactor_raft_term`
- `hpactor_raft_commit_index`
- `hpactor_raft_last_applied`
- `hpactor_raft_log_entries`
- `hpactor_raft_snapshot_index`
- `hpactor_raft_elections_total`
- `hpactor_raft_leader_changes_total`
- `hpactor_raft_append_entries_latency_seconds`
- `hpactor_raft_quorum_lost_total`

CLI/admin:

```text
/cluster raft status
/cluster raft peers
/cluster raft log
/cluster raft snapshot
/cluster raft transfer-leader <node>
/cluster raft step-down
```

Logs and audit events include term, index, role, leader id, voter config id,
singleton name, fencing token, and transition reason.

## 18. Testing Strategy

### Deterministic Unit Tests

- follower rejects stale term and stale log entries,
- candidate wins with quorum and up-to-date log,
- candidate loses when another candidate has newer log,
- leader commits only current-term entries,
- fencing token ordering follows committed term/index,
- no-quorum state rejects leadership renewal.

### Model And Simulation Tests

- randomized message reorder/drop/duplicate simulation,
- split 3/2 in five-node cluster,
- leader isolated after granting singleton lease,
- follower crash and restart after vote persistence,
- log conflict repair,
- snapshot install after compaction,
- joint consensus add/remove voter transitions.

### Integration Tests

- `RaftLeadershipBackend::try_acquire()` returns a lease only after commit,
- `renew()` returns a higher token after committed renewal,
- `release()` clears ownership after commit,
- `watch()` reports higher-token owner and local fencing,
- `ShardCoordinatorActor` rejects mutations after Raft quorum loss.

### Storage Tests

- torn write detection through CRC,
- atomic segment replacement,
- snapshot recovery with last included term/index,
- corrupt snapshot rejection,
- durable term and vote survive restart.

## 19. Migration Path

1. Land external coordinator backend first and stabilize `ILeadershipBackend`.
2. Add fake deterministic backend and leadership state-machine tests.
3. Implement `RaftNodeCore` without networking or storage.
4. Add durable `RaftLogStore` and `RaftSnapshotStore`.
5. Add deterministic transport simulation.
6. Integrate `RaftLeadershipBackend` behind `system.cluster.leadership.backend = "raft"`.
7. Add chaos, partition, restart, and long-run soak tests before marking the
   backend production-ready.

## 20. Acceptance Criteria

1. Raft grants leadership only through committed log entries.
2. Fencing tokens are monotonic for each singleton and derived from committed
   `(term, log_index)`.
3. Quorum uses the committed voter set, not local alive-node observations.
4. Joint consensus protects membership changes.
5. Loss of Raft quorum causes active singleton step-down before accepting new
   mutating work.
6. Durable term, vote, log, and snapshot state survive process restart.
7. Deterministic simulation covers partitions, dropped messages, duplicate
   messages, crash/restart, log repair, and snapshot install.
