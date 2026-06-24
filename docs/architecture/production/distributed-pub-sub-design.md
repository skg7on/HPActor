# Distributed Pub-Sub Mediator — Design Document

## 1. Executive Summary

The Distributed Pub-Sub Mediator (CLU-005) provides topic-based publish/subscribe across cluster nodes in HPActor. Actors subscribe to named topics, and published messages fan out to all subscribers cluster-wide. This closes a critical Akka parity gap (`akka-cluster-typed` distributed pub-sub).

## 2. Architecture

### 2.1 Core/Actor Separation
- **PubSubMediatorCore** — Thread-safe (mutex-guarded) core managing topic→subscriber maps. Testable without ActorSystem.
- **PubSubMediatorActor** — Delegate-to-core wrapper. Future evolution to EventBasedActor with TypeTag dispatch.

### 2.2 Data Model
- **PubSubTopic** — String-based topic identifier (e.g., `"system.health"`, `"orders.created"`).
- **TopicSubscription** — `(topic, subscriber_node, subscriber_actor_id, incarnation)` — represents a subscription binding on a specific node.

### 2.3 Capacity Bounds
- `kMaxTopics = 4096` — maximum distinct topics per mediator instance.
- `kMaxSubscribersPerTopic = 1024` — maximum subscribers per individual topic.

## 3. Subscriber Lifecycle

### Local Subscribe
1. Actor calls `PubSubMediatorCore::subscribe(topic, actor_id)`.
2. Core adds `actor_id` to `local_subs_[topic]`.
3. If capacity exceeded, returns false.
4. Successful new subscription is added to `dirty_subs_` for gossip dissemination.

### Local Unsubscribe
1. Actor calls `PubSubMediatorCore::unsubscribe(topic, actor_id)`.
2. Core removes `actor_id` from `local_subs_[topic]`.
3. If topic becomes empty, it is removed.

### Remote Subscription Merge
1. Gossip piggyback delivers remote `TopicSubscription` entries.
2. `merge_remote_subscription(sub)` adds to `remote_subs_[topic][node_id]`.
3. Higher-incarnation entries overwrite lower ones.

### Node-Down Cleanup
1. `ClusterFailureModel` triggers `remove_node_subscriptions(node_id)`.
2. All `remote_subs_` entries from that node are purged.

## 4. Gossip Dissemination

- Local subscription changes are tracked in `dirty_subs_` vector.
- Periodically, `drain_dirty_subscriptions()` flushes the dirty set.
- Dirty entries are encoded as gossip `PiggybackEntry` with `PiggybackType::Metadata`.
- On receiving gossip piggyback, remote subscription entries are merged via `merge_remote_subscription()`.

## 5. Testing

### PubSubMediatorCore (14 tests)
- Subscribe/Unsubscribe lifecycle, capacity enforcement (topic + subscriber limits), local subscriber query, remote merge, node-down cleanup, dirty subscription tracking, independent topics.

### PubSubMediatorActor (7 tests)
- Construction, subscribe/unsubscribe delegation, remote merge + query, node-down cleanup, core access.

## 6. References
- [Cluster Subsystem Architecture](../cluster/cluster-subsystem-architecture.md) — Section 10 Future Evolution
- [Feature Gap Refined Requirement Backlog](feature-gap-refined-requirement-backlog.md) — CLU-005
- [Akka Gap Analysis (Issue #329)](https://github.com/skg7on/HPActor/issues/329)
