# Cluster Receptionist — Design Document

## 1. Executive Summary

The Cluster Receptionist (CLU-006) extends the local Receptionist (Sprint 1) for cross-node ServiceKey-based actor discovery. Each node gossips its local `(ServiceKey, actor_ids)` registrations; other nodes merge them to build a cluster-wide view. When a node goes Down, its registrations are purged via `ClusterFailureModel` observer callbacks.

## 2. Architecture

### 2.1 Core/Actor Separation
- **ClusterReceptionistCore** — Thread-safe (mutex-guarded). Manages local and remote ServiceKey→actor_id mappings.
- **ClusterReceptionistActor** — Delegate-to-core wrapper. Future evolution to EventBasedActor with local Receptionist subscription and gossip integration.

### 2.2 Data Model
- **ClusterRegistration** — `(ServiceKey, node_id, actor_ids[], incarnation)` — represents a node's registrations under a ServiceKey.
- ServiceKey from `hpactor::receptionist::ServiceKey` (string-based).

## 3. Key Operations

- `apply_local_registration(key, actor_ids)` — Register local actors under a key; mark dirty for gossip.
- `merge_remote_registration(reg)` — Merge a remote node's registration; higher incarnation overwrites.
- `remove_node_registrations(node_id)` — Purge all registrations from a dead node.
- `get_cluster_listing(key)` — Combined local + remote actor IDs for a key.
- `drain_dirty_registrations()` — Flush dirty registrations for gossip dissemination.

## 4. Testing (15 tests)
- Core: Apply/merge/remove/query, incarnation conflict resolution, independent keys, dirty drain lifecycle.
- Actor: Construction, register/query, remote merge, node-down cleanup, core access.

## 5. Future Integration
- Subscribe to local Receptionist for keys with remote registrations.
- Proxy remote actor addresses so local subscribers see them in Listing messages.
- Gossip piggyback dissemination of registration state.

## 6. References
- [Cluster Subsystem Architecture](../cluster/cluster-subsystem-architecture.md) — Section 10
- [Feature Gap Refined Requirement Backlog](feature-gap-refined-requirement-backlog.md) — CLU-006
- Sprint 1: Local Receptionist (PR #344)
