.. _building-applications-actor-types:

Actor Types
===========

HPActor provides several actor base classes, each suited to different
concurrency and programming models. Choosing the right one is the first
design decision when building an HPActor application.

Actor Type Hierarchy
--------------------

.. code-block:: text

   AbstractActor (interface base)
       └── LocalActor (has ActorContext access)
               ├── EventBasedActor (cooperative, behavior-based)
               │       ├── StatefulActor<T> (explicit typed state)
               │       ├── ProtoStatefulActor<T> (protobuf-native + state)
               │       ├── SpawnReceiver (system actor)
               │       └── DenseComputingActor (dedicated pool dispatch)
               ├── TypedEventBasedActor<Signatures...> (statically typed)
               ├── BlockingActor (dedicated thread, blocking receive)
               │       └── ScopedActor (for main/non-actor contexts)
               └── DaemonActor (dedicated thread, run_once() loop)
                       ├── PollingActor (CPU affinity, poll budget)
                       ├── ExternalMsgGatewayActor (named routes)
                       │       └── net::HTTPGatewayActor (HTTP ingress)
                       └── cli::CliActor (stdin/socket I/O, command tree)

EventBasedActor
---------------

The **workhorse** of HPActor. Each instance runs cooperatively on the
shared scheduler — lightweight, fast, and the right default.

**When to use:**
- Most application actors (workers, coordinators, routers).
- Message-driven logic with protobuf handlers.
- Actors that spend most time waiting for messages.

**When NOT to use:**
- CPU-intensive work (use :cpp:class:`DenseComputingActor`).
- Blocking I/O (use :cpp:class:`BlockingActor` or async transport).

.. code-block:: cpp

   class MyWorker : public EventBasedActor {
   protected:
       Behavior make_behavior() override {
           return Behavior::make()
               .on<ProcessRequest>([this](const ProcessRequest& req) {
                   auto result = do_work(req);
                   context()->reply(ProcessResponse{result});
               });
       }
   };

StatefulActor<T>
----------------

An :cpp:class:`EventBasedActor` with explicitly typed, encapsulated state.
The state is accessible via ``state()`` and is single-threaded by
construction (no locks needed).

**When to use:**
- Actors that accumulate or transform state over time.
- Cache actors, aggregators, session actors.

.. code-block:: cpp

   struct CounterState {
       int64_t value = 0;
       int64_t last_reset_at = 0;
   };

   class CounterActor : public StatefulActor<CounterState> {
   protected:
       Behavior make_behavior() override {
           return Behavior::make()
               .on<Increment>([this](const Increment&) {
                   state().value += 1;
               })
               .on_request<GetCount, CountResponse>([this](const GetCount&) {
                   CountResponse resp;
                   resp.set_value(state().value);
                   context()->reply(resp);
               });
       }
   };

TypedEventBasedActor<Signatures...>
------------------------------------

Statically typed actor using C++ template signatures. The compiler
verifies that all handled message types are covered.

**When to use:**
- Subsystem boundaries where compile-time type safety is valuable.
- Internal APIs with well-defined message contracts.

.. code-block:: cpp

   using CalculatorActor = TypedEventBasedActor<
       AddRequest, SubRequest, MulRequest
   >;

BlockingActor
-------------

Runs on its own dedicated thread with a blocking receive loop. Each
instance corresponds to one OS thread.

**When to use:**
- Actors that perform blocking I/O (file reads, synchronous DB calls).
- Legacy code integration that can't be made async.

**When NOT to use:**
- High-concurrency workloads (use EventBasedActor instead).
- Scenarios where you'd spawn hundreds of these (threads are expensive).

.. code-block:: cpp

   class FileWriter : public BlockingActor {
   protected:
       void run() override {
           while (!shutdown_requested()) {
               auto msg = receive();  // blocks until message arrives
               // ... blocking file write ...
           }
       }
   };

ScopedActor
-----------

A convenience subclass of :cpp:class:`BlockingActor` designed for the
main thread or other non-actor contexts. Use it to send messages to
actors from ``main()`` or from external libraries.

.. code-block:: cpp

   int main() {
       ActorSystem system;
       auto ref = system.spawn<MyActor>();

       ScopedActor client(system);
       client.send(ref, SomeMessage{...});
       auto reply = client.receive<SomeResponse>();
   }

DaemonActor
-----------

A dedicated-thread actor with a ``run_once()`` loop — the actor calls
``run_once()`` in a tight loop, performing work and polling for messages.

**When to use:**
- Polling-based I/O, hardware interfaces, or custom event loops.
- Actors that need CPU affinity or poll budgets.

**Subtypes:**
- :cpp:class:`PollingActor` — adds CPU pinning and poll budget.
- :cpp:class:`ExternalMsgGatewayActor` — named route table, transforms.
- :cpp:class:`CliActor` — stdin/socket I/O, interactive command tree.

DenseComputingActor
-------------------

Dispatched to a dedicated thread pool for CPU-bound work. Unlike
:cpp:class:`EventBasedActor`, these do not run on the cooperative
scheduler — they get their own threads to avoid starving message
processing.

**When to use:**
- Heavy computation: image processing, ML inference, compression.
- Work that would block the cooperative scheduler for > 1 ms.

Decision Guide
--------------

.. list-table::
   :header-rows: 1

   * - Scenario
     - Recommended Actor Type
   * - Message-driven, I/O-bound, most common case
     - EventBasedActor / StatefulActor<T>
   * - Compile-time type safety at subsystem boundary
     - TypedEventBasedActor<Signatures...>
   * - Accumulating state (counters, caches, aggregators)
     - StatefulActor<T>
   * - Protobuf-native with typed state
     - ProtoStatefulActor<T>
   * - Blocking I/O (files, sync DB)
     - BlockingActor
   * - Non-actor context (main thread, external lib)
     - ScopedActor
   * - Polling loop, hardware interface, custom event loop
     - DaemonActor / PollingActor
   * - CPU-intensive computation
     - DenseComputingActor
   * - HTTP ingress gateway
     - HTTPGatewayActor
   * - Interactive CLI
     - CliActor
