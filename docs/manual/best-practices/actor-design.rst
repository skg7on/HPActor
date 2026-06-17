.. _best-practices-actor-design:

Actor Design Patterns
=====================

Good actor design leads to systems that are easy to reason about, test,
and scale. Poor design leads to tangled message flows, hidden coupling,
and hard-to-debug failures.

Principles
----------

1. **Single responsibility.** Each actor should do one thing well.
   If an actor handles orders, billing, and notifications, split it into
   three.

2. **Prefer fire-and-forget over request-response.** For throughput-critical
   paths, send a message and move on. Reserve ask/RPC for when you truly
   need a response.

3. **State is private.** Never share mutable state between actors. If two
   actors need the same data, either:
   - Route all writes through a single owner actor.
   - Use a cache actor with invalidation messages.
   - Use copy-on-write snapshots.

4. **Keep handler logic fast.** A message handler runs on the cooperative
   scheduler. If it takes longer than ~1ms, consider:
   - Offloading CPU work to a :cpp:class:`DenseComputingActor`.
   - Batching work and processing in chunks.
   - Moving blocking I/O to a :cpp:class:`BlockingActor`.

5. **Make supervision explicit.** Every actor should have a supervisor.
   Top-level actors are supervised by the ``ActorSystem``.

Patterns
--------

Root Actor / Coordinator
~~~~~~~~~~~~~~~~~~~~~~~~

A single coordinator actor owns the top-level logic and delegates to
specialized children:

.. code-block:: cpp

   class OrderCoordinator : public SelfSupervisingActor {
       ActorRef inventory_;
       ActorRef payment_;
       ActorRef shipping_;

       Behavior make_behavior() override {
           return Behavior::make()
               .on<PlaceOrder>([this](const PlaceOrder& order) {
                   // Delegate to children — coordinator doesn't do the work
                   context()->send(inventory_, ReserveRequest{order});
                   context()->send(payment_, ChargeRequest{order});
               });
       }
   };

Worker Pool
~~~~~~~~~~~

Spawn multiple identical workers and route to them:

.. code-block:: cpp

   class WorkerPool : public EventBasedActor {
       std::vector<ActorRef> workers_;
       size_t next_ = 0;

       Behavior make_behavior() override {
           return Behavior::make()
               .on<WorkItem>([this](const WorkItem& item) {
                   auto& target = workers_[next_++ % workers_.size()];
                   context()->send(target, item);
               });
       }
   };

Cache Actor
~~~~~~~~~~~

Maintain a local cache with TTL and invalidation:

.. code-block:: cpp

   class CacheActor : public StatefulActor<CacheState> {
       Behavior make_behavior() override {
           return Behavior::make()
               .on_request<Lookup, LookupResponse>(
                   [this](const Lookup& req) {
                       auto it = state().entries.find(req.key());
                       if (it != state().entries.end() && !it->second.expired()) {
                           context()->reply(it->second.value);
                       } else {
                           // Fetch from source and populate cache
                       }
                   })
               .on<Invalidate>([this](const Invalidate& inv) {
                   state().entries.erase(inv.key());
               });
       }
   };

Actor-Per-Entity
~~~~~~~~~~~~~~~~

Spawn one actor per domain entity (user, session, order):

.. code-block:: cpp

   class UserManager : public EventBasedActor {
       std::unordered_map<std::string, ActorRef> user_actors_;

       Behavior make_behavior() override {
           return Behavior::make()
               .on<RouteToUser>([this](const RouteToUser& msg) {
                   auto& user_ref = user_actors_[msg.user_id];
                   if (!user_ref) {
                       user_ref = context()->spawn<UserActor>(msg.user_id);
                   }
                   context()->send(user_ref, msg.payload);
               });
       }
   };

FSM with become()
~~~~~~~~~~~~~~~~~

Model explicit states using ``become()`` instead of flag variables:

.. code-block:: cpp

   // GOOD: explicit states
   become(payment_pending_behavior(order));

   // BAD: implicit state via booleans
   is_payment_pending_ = true;
   is_shipping_ready_ = false;

Anti-Patterns
-------------

**God Actor**
  One actor that does everything — becomes a bottleneck and single point
  of failure. Split into focused actors.

**Chatty Actors**
  Actors that send many small messages instead of batching. Each message
  has overhead (enqueue, dequeue, dispatch). Batch related data.

**Circular Dependencies**
  Actor A sends to B, B sends to C, C sends to A. Creates coupling and
  makes shutdown ordering fragile. Prefer tree-structured message flow.

**Shared Mutable State**
  Two actors reading/writing the same ``std::map``. This breaks actor
  isolation and introduces data races. All state should be private.

**Blocking in Event Handler**
  Calling a blocking API inside ``.on<Message>(handler)``. This starves
  other actors on the same scheduler thread. Use ``BlockingActor`` or
  async patterns instead.

**Ask for Everything**
  Using ``context()->ask()`` for every interaction. This creates
  synchronous coupling and limits throughput. Use fire-and-forget for
  notifications, events, and status updates.

Sizing Guidelines
-----------------

.. list-table::
   :header-rows: 1

   * - Concern
     - Guideline
   * - Actors per scheduler thread
     - 100–10,000 (lightweight; context switch is cooperative)
   * - Messages per second per actor
     - 1,000–100,000 (depends on handler complexity)
   * - Mailbox capacity
     - Start at 1,024; monitor and tune
   * - Worker pool size
     - 1–2× CPU cores for CPU-bound; 10–100× for I/O-bound
   * - Actor state size
     - Keep under 1MB per actor; use hibernation for cold actors
