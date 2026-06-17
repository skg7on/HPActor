.. _getting-started-overview:

Framework Overview
==================

HPActor is a C++20 event-based actor framework inspired by the C++ Actor
Framework (CAF). It provides the building blocks for constructing concurrent
and distributed systems using the **actor model** — a computational model
where "actors" are the universal primitives of computation, each with:

- **Private state** — never shared directly with other actors.
- **A mailbox** — an ordered queue of incoming messages.
- **Behavior** — a function that processes one message at a time.

Why Actors?
-----------

Traditional shared-memory concurrency with threads and locks is error-prone
at scale. The actor model addresses this by enforcing:

- **Isolation** — actors communicate only through message passing.
- **Single-threaded execution** — each actor processes messages sequentially.
- **Location transparency** — an actor reference abstracts whether the target
  is local or remote.
- **Supervision** — parent actors manage the lifecycle and failure of children.

These properties make actor-based systems easier to reason about, test, and
scale across cores and machines.

HPActor Architecture at a Glance
--------------------------------

.. code-block:: text

   ┌─────────────────────────────────────────────────┐
   │                  ActorSystem                     │
   │  ┌──────────┐  ┌───────────┐  ┌──────────────┐  │
   │  │ Scheduler │  │ Registry  │  │   Transport   │  │
   │  └──────────┘  └───────────┘  └──────────────┘  │
   │  ┌──────────┐  ┌───────────┐  ┌──────────────┐  │
   │  │  Metrics  │  │  Tracing   │  │   Logging     │  │
   │  └──────────┘  └───────────┘  └──────────────┘  │
   └─────────────────────────────────────────────────┘
            │              │              │
       ┌────▼──┐      ┌───▼───┐     ┌───▼────┐
       │ Actor │      │ Actor │     │ Actor  │
       │  (A)  │◄────►│  (B)  │◄───►│  (C)   │
       └───────┘      └───────┘     └────────┘
         msg              msg           msg

An :cpp:class:`ActorSystem` owns the scheduler, actor registry, transport
layer, and observability subsystems. Actors are spawned within the system
and communicate exclusively through messages.

Key Concepts
------------

Actors
~~~~~~

An **actor** is a lightweight concurrent entity identified by a unique
:cpp:type:`ActorId`. HPActor supports several actor flavors:

- :cpp:class:`EventBasedActor` — cooperative, message-driven (most common).
- :cpp:class:`StatefulActor<T>` — event-based with explicit typed state.
- :cpp:class:`TypedEventBasedActor` — statically typed handlers (template
  parameter pack ``Signatures...``).
- :cpp:class:`BlockingActor` — dedicated thread with blocking receive.
- :cpp:class:`DaemonActor` — dedicated thread with a ``run_once()`` loop.

See :doc:`/building-applications/actor-types` for detailed guidance.

Messages
~~~~~~~~

Actors exchange messages. HPActor uses Protocol Buffers for wire-compatible
serialization but also supports in-memory message passing:

.. code-block:: cpp

   // Send a message to another actor
   context()->send(target_addr, my_message);

   // Reply to the current sender
   context()->reply(response);

   // Reply with an error
   context()->reply_with_error(error_code);

Supervision
~~~~~~~~~~~

Actors form **supervision trees**. A parent actor supervises its children
and decides how to handle their failures:

- **OneForOne** — restart only the failed child.
- **AllForOne** — restart all children when any fails.

.. code-block:: cpp

   class MySupervisor : public SupervisorActor {
       void on_child_failure(ActorId child, const error& err) override {
           restart_child(child);  // or stop, escalate, etc.
       }
   };

Production Reliability Plane
-----------------------------

HPActor organizes production features into three planes:

.. list-table::
   :header-rows: 1

   * - Plane
     - Concerns
     - Status
   * - **Data Plane**
     - Delivery semantics, mailbox admission, DLQ, reliable messaging,
       tracing, actor lifecycle
     - Foundation implemented
   * - **Control Plane**
     - Cluster failure model, node identity, sharding, placement,
       graceful shutdown, rolling upgrades
     - Partial (shutdown, lifecycle done; sharding/cluster in design)
   * - **Operations Plane**
     - Health endpoints, admin API, security, audit, config reload,
       chaos/soak testing
     - Partial (health, fault injection done; admin API/security in design)

For the detailed roadmap, see the production reliability architecture
docs at ``docs/architecture/production/production-reliability-plane.md``.

Where to Go Next
----------------

- :doc:`installation` — build HPActor and link against your project.
- :doc:`your-first-actor` — write and run your first actor in 5 minutes.
- :doc:`/building-applications/actor-types` — choose the right actor for
  each task.
