.. _building-applications-lifecycle:

Actor Lifecycle & Supervision
==============================

Actors in HPActor have a well-defined lifecycle and form hierarchical
supervision trees. This chapter covers spawning, linking, supervising,
and shutting down actors.

Actor Lifecycle States
----------------------

.. code-block:: text

   Created ──► Starting ──► Running ──► Stopping ──► Stopped
                   ▲             │
                   │             │ (failure)
                   │             ▼
                   └────────── Failed ──► Terminated

The :cpp:class:`LifecycleActor` mixin adds a lifecycle state machine to
any actor:

.. code-block:: cpp

   class MyManagedActor : public StatefulActor<MyState>,
                          public LifecycleActor {
       // Lifecycle transitions are compile-time verified via
       // constexpr StateDef tables.
   };

Key states:

- **Created** — actor constructed, not yet receiving messages.
- **Starting** — transition from Created; initialization in progress.
- **Running** — actively processing messages.
- **Stopping** — drain in progress; no new messages accepted.
- **Stopped** — drain complete; actor exits.
- **Failed** — unrecoverable error; supervision may restart.
- **Terminated** — final state; actor removed from registry.

The **message gate** rejects incoming messages during non-Active states
(Created, Starting, Stopping, Stopped, Failed).

Spawning Actors
---------------

Actors are always spawned by a parent — either the :cpp:class:`ActorSystem`
or another actor:

.. code-block:: cpp

   // From ActorSystem (top-level actors)
   auto ref = system.spawn<WorkerActor>();

   // From within an actor (child actors — supervision tree)
   class SupervisorActor : public EventBasedActor {
       Behavior make_behavior() override {
           return Behavior::make()
               .on<StartWorkers>([this](const StartWorkers&) {
                   for (int i = 0; i < 4; i++) {
                       auto child = context()->spawn<WorkerActor>();
                       children_.push_back(child);
                   }
               });
       }
   };

Linking and Monitoring
----------------------

Actors can establish death-awareness relationships:

.. code-block:: cpp

   // Bidirectional: if either dies, the other is notified
   context()->link_to(other_actor);
   context()->unlink_from(other_actor);

   // Unidirectional: monitor another actor's lifecycle
   context()->monitor(other_actor);
   context()->demonitor(other_actor);

When a linked or monitored actor exits, the watching actor receives a
``DownMsg``:

.. code-block:: cpp

   .on_system<DownMsg>([this](const DownMsg& msg) {
       std::cout << "Actor " << msg.actor_id
                 << " terminated: " << msg.reason << std::endl;
       // Remove from children, clean up, etc.
   });

Supervision
-----------

HPActor provides three supervision strategies:

OneForOne
~~~~~~~~~

Only the failed child is restarted:

.. code-block:: cpp

   class MySupervisor : public EventBasedActor {
       OneForOneSupervisor supervisor_;

       Behavior make_behavior() override {
           supervisor_.set_policy(3,       // max restarts
                                  std::chrono::seconds(10));  // time window
           return Behavior::make()
               .on<ChildFailed>([this](const ChildFailed& msg) {
                   supervisor_.on_child_failure(msg.actor_id, msg.error);
               });
       }
   };

AllForOne
~~~~~~~~~

All children restart if any fails — useful when children share state
or depend on each other:

.. code-block:: cpp

   AllForOneSupervisor supervisor_;
   supervisor_.set_policy(5, std::chrono::seconds(30));

SelfSupervisingActor
~~~~~~~~~~~~~~~~~~~~

An actor that manages its own children with a built-in supervisor:

.. code-block:: cpp

   class SelfManagedCoordinator : public SelfSupervisingActor {
   protected:
       Behavior make_behavior() override {
           set_supervision_policy(3, std::chrono::seconds(10));
           // Spawn children; failures are handled automatically.
       }
   };

Escalation to Quarantine
~~~~~~~~~~~~~~~~~~~~~~~~

If a child repeatedly fails, the supervisor can escalate to quarantine
instead of restarting in a loop:

.. code-block:: cpp

   QuarantinePolicy policy;
   policy.enabled = true;
   policy.max_restarts = 10;
   policy.trip_threshold = 5;  // consecutive failures to trip

   supervisor_.set_quarantine_policy(policy);

Graceful Shutdown
-----------------

HPActor supports orderly shutdown with drain policies:

.. list-table::
   :header-rows: 1

   * - Policy
     - Behavior
   * - ``Complete``
     - Process all in-flight messages before stopping.
   * - ``Drop``
     - Discard all pending messages immediately.
   * - ``Timeout``
     - Drain for a deadline; drop remaining messages after.

Configure drain per-actor or system-wide via TOML:

.. code-block:: toml

   [system.shutdown]
   drain_timeout_ms = 5000
   stop_timeout_ms = 10000

   [[actors]]
   name = "critical-worker"
   behavior = "worker"
   drain_policy = "complete"
   drain_timeout_ms = 10000

Trigger shutdown programmatically:

.. code-block:: cpp

   system.shutdown();        // Phase-machine: user actors, then system actors
   system.await_shutdown();  // Block until all actors have stopped

CLI-initiated shutdown:

.. code-block:: text

   /system drain          # Begin draining all actors
   /system stop           # Finalize shutdown

For detailed shutdown semantics, see :doc:`/best-practices/deployment`.
