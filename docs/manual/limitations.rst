.. _limitations:

Limitations & Known Gaps
========================

This chapter honestly documents the current limitations of HPActor.
These are not bugs — they are design decisions, items on the roadmap,
or trade-offs that users should understand before adopting HPActor for
a specific use case.

Production Reliability Gaps
---------------------------

Durable Outbox/Inbox (Not Implemented)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Status:** Design exists (``docs/architecture/production/reliable-messaging-design.md``).
Not implemented.

**Impact:** At-least-once delivery relies on in-memory state. If an actor
crashes or a node fails before a message is processed, the message may be
lost. There is no persistent outbox that survives restarts.

**Workaround:** Use the DLQ as a safety net — messages that fail delivery
are captured. For critical workflows, implement application-level
acknowledgment and retry.

ACK/NACK Retry Protocol (Not Implemented)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Status:** Design exists. Not implemented.

**Impact:** The RPC channel has built-in retry, but general actor-to-actor
messaging has no automatic ACK/NACK protocol. Senders are not
automatically notified of delivery success or failure.

**Workaround:** Use the ask pattern (``context()->ask()``) for
request-response with timeout, or manually implement acknowledgment
messages.

Cluster Control Plane (Partial)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Status:** Graceful shutdown and actor lifecycle are implemented. The
following are design/backlog only:

- **Node fencing** — no mechanism to prevent split-brain nodes from
  acting after partition.
- **Sharding** — no dynamic shard placement, rebalancing, or handoff.
- **Placement** — actors are spawned on a specific node; no scheduler-
  driven placement across a cluster.
- **Rolling upgrades** — no protocol negotiation or version compatibility
  checks.

**Impact:** HPActor works well for single-node and statically-configured
multi-node deployments. It is not yet suitable for fully dynamic clusters
with automatic rebalancing.

Security (Not Implemented)
--------------------------

**Status:** Design exists (``docs/architecture/production/security-architecture-design.md``).
Not implemented.

- **mTLS**: Inter-node communication is plain TCP by default. TLS is
  available at the connection level but mTLS with certificate-based
  identity is not wired into the actor system.
- **Authorization**: No role-based or attribute-based access control for
  actor operations.
- **Audit logging**: Structured logging captures events but does not
  produce a dedicated tamper-resistant audit trail.

**Workaround:** Deploy behind a service mesh (Istio, Linkerd) for mTLS.
Use network policies to restrict inter-node communication to trusted
networks.

Operations Plane (Partial)
--------------------------

.. list-table::
   :header-rows: 1

   * - Feature
     - Status
     - Workaround
   * - Health/readiness endpoints
     - ✅ Implemented
     - ``/healthz``, ``/readyz`` via HealthHttpServer
   * - Metrics (Prometheus)
     - ✅ Implemented
     - OpenMetrics endpoint via HTTP gateway
   * - Admin API
     - ❌ Not implemented
     - Use CLI over UDS/TCP for administration
   * - Dynamic config reload
     - ❌ Not implemented
     - Restart process with new config
   * - Config validation/diff
     - ❌ Not implemented
     - Validate TOML syntax externally; no semantic diff
   * - Incident timeline
     - ❌ Not implemented
     - Correlate logs, metrics, and traces manually

Proactor Backend (Not Production-Hardened)
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Status:** The ``IoUringBackend`` and ``GcdBackend`` exist but need
production hardening. The default ``EpollBackend`` (Linux) and
``KqueueBackend`` (macOS) are stable.

Platform Support
----------------

.. list-table::
   :header-rows: 1

   * - Platform
     - Status
     - Notes
   * - Linux (x86_64)
     - ✅ Full support
     - Primary development platform
   * - Linux (ARM64)
     - ✅ Supported
     - CI-tested
   * - macOS (ARM64)
     - ✅ Supported
     - kqueue backend
   * - macOS (x86_64)
     - ✅ Supported
     - kqueue backend
   * - Windows
     - ❌ Not supported
     - No WSL or native support

Scalability Limits
------------------

.. list-table::
   :header-rows: 1

   * - Dimension
     - Practical Limit
     - Bottleneck
   * - Actors per node
     - ~100,000
     - Memory, scheduler overhead
   * - Messages per second (single actor)
     - ~100,000
     - Handler complexity
   * - Messages per second (node total)
     - ~1,000,000
     - Scheduler threads, memory bandwidth
   * - Nodes in gossip cluster
     - ~100
     - SWIM protocol fanout, suspicion timeout
   * - Concurrent connections (TCP transport)
     - ~10,000
     - File descriptors, event loop throughput

These are practical estimates, not hard limits. Actual performance
depends on hardware, message size, and handler logic.

Wire Protocol
-------------

- **Protobuf-only**: The wire format requires Protocol Buffers. There is
  no pluggable serialization for non-protobuf message types.
- **No schema evolution**: TypeTag and protobuf schema changes must be
  manually managed. Adding a field is safe (protobuf compatibility);
  removing or renumbering a TypeTag is a breaking change.
- **No version negotiation**: All nodes must use the same protobuf schema
  version. There is no protocol negotiation during connection handshake.

Language Bindings
-----------------

Python (Alpha)
^^^^^^^^^^^^^^

HPActor provides an official alpha Python binding for CPython 3.11 and newer
on manylinux_2_28 x86_64/ARM64 and macOS 12.0 x86_64/ARM64.  The binding uses
generated protobuf messages with explicit ``TypeTag`` values for deterministic
serialization.  Python actors run on a dedicated asyncio event-loop thread;
the C++ scheduler and network threads never call Python or acquire the GIL.

See the :doc:`Python Binding manual <python/index>` and
`issue #426 <https://github.com/skg7on/HPActor/issues/426>`_ for
compatibility details.

**Limitations in Phase 1D:**

- Windows, musllinux, PyPy, and free-threaded CPython are not supported.
- Native remote-node participation (cluster gossip, remote spawn, distributed
  actors) is deferred until identity, authorization, and protocol negotiation
  exist.
- Declarative TOML topology for Python actors is planned for Phase 1E.

Other Languages
^^^^^^^^^^^^^^^

Java, Go, Rust, and other languages remain unsupported.  Non-C++ and
non-Python services must interact via the HTTP gateway or CLI, not as
native actors.

Coroutine Limitations
---------------------

- HPActor uses C++20 coroutines internally for actor suspension. Custom
  coroutine usage (user-defined ``co_await`` patterns, generator actors)
  is possible but not yet documented or supported through a public API.
- The coroutine frame pool is sized at startup. If all frames are in use,
  new coroutine suspensions will fail (not grow dynamically).

Test Coverage
-------------

While the core framework, networking, memory, and production subsystems
are well-tested (~2,200 GTest cases across 219 files), coverage is not
uniform:

- **High coverage**: Actor core, mailbox, memory, scheduler, metrics,
  logging, tracing, CLI.
- **Moderate coverage**: Network transport, service discovery, RPC,
  fault injection.
- **Lower coverage**: Proactor backend, HTTP gateway edge cases,
  multi-node gossip scenarios.

Roadmap
-------

These limitations are tracked on the project roadmap. The canonical
backlog is in ``docs/architecture/production/feature-gap-refined-requirement-backlog.md``.

Near-term priorities (from the production reliability plane):

1. Reliable messaging (ACK/NACK, retry, durable outbox/inbox).
2. Durable actor state (snapshot, event sourcing, recovery).
3. Health/readiness/liveness endpoint completion.
4. Security architecture implementation (mTLS, authorization, audit).
5. Dynamic config validation, diff, and reload.
6. Cluster sharding, placement, and rebalancing.
7. Proactor backend production hardening.
8. Chaos, soak, fuzz, and compatibility test lanes.

For the most up-to-date status, see the GitHub issues and project board.
