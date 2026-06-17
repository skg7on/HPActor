.. _best-practices-testing:

Testing Strategies
==================

HPActor mandates deterministic, reproducible tests. This chapter covers
patterns for testing actors, mailboxes, schedulers, and distributed
interactions.

Test Tiers
----------

.. list-table::
   :header-rows: 1

   * - Tier
     - Location
     - Scope
     - Speed
   * - **Unit**
     - ``tests/unit/``
     - Single component, no scheduler
     - < 1ms
   * - **Integration**
     - ``tests/integration/``
     - Actor interactions, config, network
     - < 100ms
   * - **System**
     - ``tests/system/``
     - End-to-end, multi-actor, fault injection
     - < 5s

Principles
----------

**No timing assumptions.** Never assume a timer fires within N ms:

.. code-block:: cpp

   // BAD: assumes scheduler processes within 100ms
   std::this_thread::sleep_for(std::chrono::milliseconds(100));
   ASSERT_TRUE(actor_processed());

   // GOOD: poll with a generous timeout
   auto deadline = Clock::now() + std::chrono::seconds(5);
   while (!actor_processed() && Clock::now() < deadline) {
       std::this_thread::yield();
   }
   ASSERT_TRUE(actor_processed());

**No thread-order assumptions.** Use ``scheduler_threads = 0`` when
inspecting intermediate state:

.. code-block:: cpp

   // For tests that need deterministic execution order:
   ActorSystem system(ActorSystemConfig{}.with_scheduler_threads(0));

**Use inject_for_test().** Seed a mailbox directly to avoid races:

.. code-block:: cpp

   auto* mailbox = actor->mailbox();
   mailbox->inject_for_test(env1);
   mailbox->inject_for_test(env2);

   // Now all messages are in the mailbox; process one at a time
   actor->process_next_message();
   ASSERT_EQ(state.before, expected_before);

   actor->process_next_message();
   ASSERT_EQ(state.after, expected_after);

Testing Actors
--------------

**Unit test an actor's behavior:**

.. code-block:: cpp

   TEST(WorkerActorTest, ProcessesWorkItem) {
       ActorSystem system(ActorSystemConfig{}.with_scheduler_threads(0));
       auto ref = system.spawn<WorkerActor>();

       // Send a message
       ScopedActor client(system);
       client.send(ref, WorkItem{.id = 1, .data = "test"});

       // Verify the actor processed it
       // (via InspectState or a query message)
       auto resp = client.ask<WorkerState>(ref, InspectRequest{});
       ASSERT_EQ(resp->processed_count, 1);
   }

**Test supervision with fault injection:**

.. code-block:: cpp

   TEST(SupervisorTest, RestartsFailedChild) {
       ActorSystem system;
       auto supervisor = system.spawn<TestSupervisor>();
       auto child = system.spawn<FlakyWorker>();

       // Use FAULT_INJECT to trigger a failure in the child
       auto schedule = FaultSchedule::builder()
           .at("hpactor.actor.process", FaultAction::Fail)
           .on_tick(1)
           .build();
       system.fault_controller().load_schedule(schedule);
       system.fault_controller().enable();

       // Wait for restart via polling
       auto deadline = Clock::now() + std::chrono::seconds(5);
       bool restarted = false;
       while (Clock::now() < deadline) {
           if (supervisor->restart_count() > 0) {
               restarted = true;
               break;
           }
           std::this_thread::yield();
       }
       ASSERT_TRUE(restarted);
   }

Testing Mailboxes
-----------------

.. code-block:: cpp

   TEST(BoundedMailboxTest, OverflowToDLQ) {
       auto dlq = std::make_shared<DeadLetterQueue>(100);
       BoundedMailbox<Envelope> mailbox(10, OverflowPolicy::DeadLetterQueue, dlq);

       // Fill the mailbox
       for (int i = 0; i < 10; i++) {
           ASSERT_TRUE(mailbox.try_enqueue(make_envelope(i)));
       }

       // 11th message should overflow to DLQ
       auto result = mailbox.try_enqueue(make_envelope(11));
       ASSERT_EQ(result.code(), EnqueueResultCode::Overflow);
       ASSERT_EQ(dlq->record_count(), 1);
   }

Testing Schedulers
------------------

Use the deterministic scheduler control API:

.. code-block:: cpp

   TEST(SchedulerTest, DispatchOrder) {
       SchedulerTestDriver driver(/*worker_count=*/1);

       auto actor_a = driver.spawn<TestActor>("a");
       auto actor_b = driver.spawn<TestActor>("b");

       driver.send(actor_a, TestMessage{.seq = 1});
       driver.send(actor_b, TestMessage{.seq = 2});

       // Step one message at a time
       driver.step();  // Process actor_a
       ASSERT_TRUE(actor_a->last_seq() == 1);
       ASSERT_TRUE(actor_b->last_seq() == 0);  // Not yet

       driver.step();  // Process actor_b
       ASSERT_TRUE(actor_a->last_seq() == 1);
       ASSERT_TRUE(actor_b->last_seq() == 2);
   }

The ``SchedulerTestDriver`` (from ``scheduler_test_driver.hpp``) provides
``pause()``, ``resume()``, ``step()``, and ``step_n()`` for fine-grained
execution control.

Testing Network Code
--------------------

Use the loopback transport for deterministic network tests:

.. code-block:: cpp

   TEST(NetworkTest, RemoteMessageRoundtrip) {
       ActorSystem node_a(ActorSystemConfig{}.with_loopback_transport());
       ActorSystem node_b(ActorSystemConfig{}.with_loopback_transport());

       // Wire them together (no real sockets)
       node_a.transport().connect_loopback(node_b.transport());

       auto ref_a = node_a.spawn<EchoActor>();
       auto ref_b = node_b.resolve(ref_a.id());

       // Send from B to A
       ScopedActor client(node_b);
       client.send(ref_b, StringMsg{"hello"});
       auto reply = client.receive<StringMsg>();
       ASSERT_EQ(reply->text, "hello");
   }

Sanitizer Testing
-----------------

.. code-block:: bash

   # ThreadSanitizer — catches data races
   cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON
   ninja -C build-tsan
   ctest --test-dir build-tsan --parallel 8

   # AddressSanitizer — catches use-after-free, buffer overflows
   cmake -S . -B build-asan -GNinja -DENABLE_ASAN=ON
   ninja -C build-asan
   ctest --test-dir build-asan --parallel 8

.. note::

   ASAN may report false positives in mailbox awaiter and priority
   scheduler tests due to intrusive queue memory patterns.

Fault Injection Tests
---------------------

Test resilience with deterministic fault injection:

.. code-block:: cpp

   TEST(ChaosTest, SurvivesMailboxDrops) {
       auto schedule = FaultSchedule::builder()
           .at("hpactor.mailbox.enqueue", FaultAction::Drop)
               .with_probability(0.5)
               .build();

       system.fault_controller().load_schedule(schedule);
       system.fault_controller().enable();

       // Send 1000 messages — 50% will be dropped
       // Verify system is still healthy and DLQ captured the drops
       ASSERT_GT(system.dead_letter_queue().record_count(), 0);
       ASSERT_TRUE(system.is_healthy());  // System didn't crash
   }

CI Pipeline
-----------

Recommended CI stages (fast to slow):

.. code-block:: yaml

   # .github/workflows/tests.yml
   jobs:
     unit:
       runs-on: ubuntu-latest
       steps:
         - run: cmake -S . -B build -GNinja
         - run: ninja -C build
         - run: ctest --test-dir build -L unit --parallel 8

     integration:
       needs: unit
       steps:
         - run: ctest --test-dir build -L integration --parallel 8

     system:
       needs: integration
       steps:
         - run: ctest --test-dir build -L system --parallel 4

     sanitizers:
       needs: unit
       steps:
         - run: cmake -S . -B build-tsan -DENABLE_TSAN=ON
         - run: ninja -C build-tsan
         - run: ctest --test-dir build-tsan --parallel 8
