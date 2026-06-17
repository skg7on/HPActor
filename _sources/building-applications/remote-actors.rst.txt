.. _building-applications-remote-actors:

Remote Actors & Service Discovery
==================================

HPActor supports multi-node actor systems with location-transparent
message passing, remote spawn, and pluggable service discovery.

Location Transparency
---------------------

An :cpp:type:`ActorRef` abstracts whether the target is local or remote:

.. code-block:: cpp

   ActorRef ref = system.spawn<Worker>();        // local
   ActorRef remote = resolve("worker-1@node-b"); // remote (via discovery)

   // Same API regardless of location:
   context()->send(ref, message);
   context()->send(remote, message);

Under the hood:
- **Local**: ``ActorRef`` holds a shared_ptr to the local actor.
- **Remote**: ``ActorRef`` holds an ``ActorProxy`` for transport via :cpp:class:`TcpTransport`.

Service Discovery
-----------------

HPActor provides four discovery backends behind the
:cpp:class:`IServiceDiscovery` interface:

.. list-table::
   :header-rows: 1

   * - Backend
     - Protocol
     - Best For
   * - ``UdpRegistrar``
     - UDP broadcast + TCP registration
     - Single-host, same-machine clusters
   * - ``GossipMembership``
     - SWIM gossip protocol
     - Cross-server, decentralized, up to ~100 nodes
   * - ``StaticDiscovery``
     - Fixed configuration file
     - Known, stable topologies
   * - ``HybridDiscovery``
     - Composes UDP + Gossip
     - Mixed deployments (local + remote)

Configuration via TOML:

.. code-block:: toml

   [system.discovery]
   backend = "gossip"

   [system.discovery.gossip]
   gossip_port = 6000
   seeds = ["10.0.1.1:6000", "10.0.1.2:6000"]
   protocol_period_ms = 1000
   ping_timeout_ms = 200
   suspicion_timeout_ms = 3000
   dead_timeout_ms = 30000
   fanout = 3

Gossip (SWIM) Protocol
~~~~~~~~~~~~~~~~~~~~~~

The :cpp:class:`GossipMembership` backend implements the SWIM protocol:

1. **Ping** — each node periodically pings a random peer.
2. **Ack** — the target responds; silence triggers indirect probes.
3. **PingReq** — the prober asks ``fanout`` other peers to ping the
   suspect on its behalf.
4. **Suspicion** — if indirect probes also fail, the node is marked
   *suspicious*. It has ``suspicion_timeout_ms`` to refute.
5. **Death** — after ``dead_timeout_ms`` without refutation, the node is
   declared dead and :cpp:class:`ActorSystem::on_node_dead()` propagates
   ``DownMsg`` to all linked/monitoring actors.

Incarnation numbers prevent split-brain resurrection.

ActorLocationCache
------------------

:cpp:class:`ActorLocationCache` maintains a TTL cache of
``ActorId → EndPoint`` mappings to avoid discovery lookups on every send:

.. code-block:: cpp

   // Automatic — ActorProxy::send() checks the cache first.
   // Manual pre-warming:
   system.location_cache().insert(actor_id, endpoint, std::chrono::seconds(60));

   // Purge entries for a dead node:
   system.on_node_dead(node_id);  // cleans cache automatically

Remote Spawn
------------

Spawn an actor on a remote node:

.. code-block:: cpp

   // Async spawn (returns immediately)
   AsyncActor future = system.spawn_remote_async(
       "node-b:5000",
       "worker",
       spawn_args
   );

   // Wait for the result
   auto result = future.get();  // blocks until spawn completes or times out
   if (result) {
       ActorRef remote_actor = *result;
   }

   // Blocking spawn
   auto remote_actor = system.spawn_remote("node-b:5000", "worker", args);

Under the hood:
1. ``SpawnRequest`` is serialized and sent to the remote node.
2. The remote ``SpawnReceiver`` system actor deserializes and spawns.
3. ``SpawnResponse`` returns the new ``ActorId`` to the caller.

ActorRef Resolution
-------------------

Resolve an actor by name from any node:

.. code-block:: cpp

   ActorRef ref = system.resolve("coordinator@node-b");

   // send() on resolved refs is location-transparent:
   context()->send(ref, message);

Considerations
--------------

- **Network partitions**: The gossip backend will mark unreachable nodes
  as dead after ``dead_timeout_ms``. Applications should handle
  ``DownMsg`` for all remote dependencies.
- **Message ordering**: HPActor uses a single TCP connection per
  node pair — messages between two specific actors on the same node pair
  are ordered (TCP semantics). Messages between different node pairs
  have no ordering guarantees.
- **Serialization overhead**: Remote messages are serialized via
  protobuf. For latency-sensitive local communication, keep actors
  on the same node.
