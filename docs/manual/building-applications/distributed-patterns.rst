.. _building-applications-distributed-patterns:

Distributed Patterns
====================

This chapter describes common distributed system patterns implemented
with HPActor primitives.

Ask Pattern (Request-Response)
------------------------------

Send a request and receive a typed response, with timeout handling:

.. code-block:: cpp

   // From an actor:
   auto handle = context()->ask<OrderResponse>(
       order_actor,
       OrderRequest{...},
       RequestTimeout::after(std::chrono::seconds(5))
   );

   // The AskManager correlates responses by MessageId.
   if (handle.ready()) {
       auto resp = handle.get();
       if (resp.has_value()) {
           process(resp.value());
       }
   } else {
       // Timeout — emit failure or retry
   }

**When to use:** Querying another actor for data, RPC-style interactions.

**Trade-off:** The ask pattern introduces coupling — the caller blocks
(when calling ``get()``) or polls ``ready()``. Prefer fire-and-forget
with callback messages for high-throughput paths.

Scatter-Gather
--------------

Fan out a request to multiple workers and aggregate results:

.. code-block:: cpp

   class ScatterGatherCoordinator : public StatefulActor<GatherState> {
       Behavior make_behavior() override {
           return Behavior::make()
               .on<FanOutRequest>([this](const FanOutRequest& req) {
                   state().expected_responses = workers_.size();
                   state().responses.clear();

                   for (auto& worker : workers_) {
                       context()->send(worker, req);
                   }
               })
               .on<WorkerResponse>([this](const WorkerResponse& resp) {
                   state().responses.push_back(resp);
                   if (state().responses.size() >= state().expected_responses) {
                       auto aggregated = aggregate(state().responses);
                       context()->reply(aggregated);
                   }
               });
       }
   };

**When to use:** Parallel processing, map-reduce, search across shards.

Router Pattern
--------------

A router actor distributes work to a pool of workers:

.. code-block:: cpp

   class RoundRobinRouter : public EventBasedActor {
       std::vector<ActorRef> workers_;
       size_t next_ = 0;

       Behavior make_behavior() override {
           return Behavior::make()
               .on<WorkItem>([this](const WorkItem& item) {
                   // Round-robin dispatch
                   auto target = workers_[next_];
                   next_ = (next_ + 1) % workers_.size();
                   context()->send(target, item);
               });
       }
   };

**Variations:**
- **Random** — load-balance with ``std::uniform_int_distribution``.
- **Least-loaded** — track pending work per worker; route to the minimum.
- **Consistent hash** — route by key for sticky session affinity.

Pub-Sub (Broadcast)
-------------------

Notify multiple subscribers of an event:

.. code-block:: cpp

   class Publisher : public EventBasedActor {
       std::vector<ActorRef> subscribers_;

       Behavior make_behavior() override {
           return Behavior::make()
               .on<Subscribe>([this](const Subscribe& sub) {
                   subscribers_.push_back(sub.subscriber);
                   context()->monitor(sub.subscriber);  // auto-remove on death
               })
               .on<PublishEvent>([this](const PublishEvent& event) {
                   for (auto& sub : subscribers_) {
                       context()->send(sub, event);
                   }
               });
       }
   };

**When to use:** Configuration changes, health updates, cache invalidation.

Work Pull (Competing Consumers)
-------------------------------

Workers pull work when ready — avoids overwhelming a single worker:

.. code-block:: cpp

   class WorkQueue : public EventBasedActor {
       std::deque<WorkItem> queue_;

       Behavior make_behavior() override {
           return Behavior::make()
               .on<SubmitWork>([this](const SubmitWork& item) {
                   queue_.push_back(item);
               })
               .on_request<RequestWork, WorkItem>([this](const RequestWork&) {
                   if (!queue_.empty()) {
                       auto item = queue_.front();
                       queue_.pop_front();
                       context()->reply(item);
                   } else {
                       context()->reply_with_error(error::empty);
                   }
               });
       }
   };

**When to use:** Dynamic worker pools, backpressure-aware processing.

Sharding (Conceptual)
---------------------

While cluster sharding is in the design/backlog phase, local sharding by
key hash is straightforward:

.. code-block:: cpp

   class LocalShardRouter : public EventBasedActor {
       std::vector<ActorRef> shards_;

       ActorRef shard_for(const std::string& key) {
           size_t h = std::hash<std::string>{}(key);
           return shards_[h % shards_.size()];
       }

       Behavior make_behavior() override {
           return Behavior::make()
               .on<KeyedRequest>([this](const KeyedRequest& req) {
                   context()->send(shard_for(req.key()), req);
               });
       }
   };

**Limitations:**
- Shards are fixed at spawn time — no dynamic rebalancing.
- No handoff protocol for shard migration.
- Remote sharding requires cluster control plane (backlog).

Actor-Driven Finite State Machines (FSM)
-----------------------------------------

Use ``become()`` to model explicit state transitions:

.. code-block:: cpp

   class OrderFSM : public EventBasedActor {
       Behavior make_behavior() override {
           return Behavior::make()
               .on<PlaceOrder>([this](const PlaceOrder& order) {
                   // Process payment...
                   become(payment_pending_behavior(order));
               });
       }

       Behavior payment_pending_behavior(const PlaceOrder& order) {
           return Behavior::make()
               .on<PaymentConfirmed>([this](const PaymentConfirmed&) {
                   // Ship order...
                   become(shipping_behavior());
               })
               .on<PaymentFailed>([this](const PaymentFailed&) {
                   become(make_behavior());  // Reset to initial
               });
       }
   };

**Benefits:** Explicit states, no bool flags, easy to reason about.

Workflow Composition
--------------------

Chain actors for multi-step workflows:

.. code-block:: cpp

   class WorkflowEngine : public EventBasedActor {
       Behavior make_behavior() override {
           return Behavior::make()
               .on<StartWorkflow>([this](const StartWorkflow& wf) {
                   // Step 1: Validate
                   auto handle = context()->ask<ValidationResult>(
                       validator_, ValidateRequest{wf.data});
                   pending_[handle.message_id()] = {wf, 1};
               })
               .on<ValidationResult>([this](const ValidationResult& r) {
                   // Step 2: Process (only if validation passed)
                   if (r.valid()) {
                       context()->send(processor_, ProcessRequest{...});
                   }
               });
       }
   };

For more complex workflows, consider the durable outbox/inbox pattern
(design/backlog — see :doc:`/limitations`).
