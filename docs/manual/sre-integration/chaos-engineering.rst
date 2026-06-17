.. _sre-integration-chaos-engineering:

Chaos Engineering
=================

HPActor includes a deterministic fault injection framework for chaos
engineering. With 80 fault injection sites across 14 domains, you can
simulate failures in a controlled, reproducible manner.

Fault Injection Architecture
-----------------------------

.. code-block:: text

   ┌──────────────────────────────────────────┐
   │           FaultController                 │
   │  ┌──────────────┐  ┌──────────────────┐  │
   │  │FaultSchedule │  │FaultPointRegistry│  │
   │  │(pre-computed)│  │(global trie)     │  │
   │  └──────────────┘  └──────────────────┘  │
   └──────────────────────────────────────────┘
            │                    │
   ┌────────▼──────┐    ┌───────▼────────┐
   │ FAULT_INJECT  │    │ FAULT_INJECT   │
   │ (mailbox)     │    │ (transport)    │
   └───────────────┘    └────────────────┘

Key components:
- :cpp:class:`FaultController` — runtime opt-in, owned by ActorSystem.
- :cpp:class:`FaultSchedule` — pre-computed (domain, tick, path, action, target) schedule.
- :cpp:class:`FaultPointRegistry` — global trie of registered fault injection sites.

Injecting Faults
----------------

Faults are injected at compile-time-defined sites using the
``FAULT_INJECT(path)`` macro:

.. code-block:: cpp

   // In mailbox enqueue path:
   bool MPSCActorMailbox::try_enqueue(Envelope* env) {
       FAULT_INJECT("hpactor.mailbox.enqueue");
       // ... normal enqueue logic
   }

At runtime, the macro:
1. Checks the ``FaultSchedule`` for a match at the current domain tick.
2. If matched, applies the configured action: Drop, Delay, Fail, Corrupt, or Panic.
3. Predicted cold via ``HPACTOR_UNLIKELY`` — zero overhead when disabled.

Fault Domains (14 total)
------------------------

.. list-table::
   :header-rows: 1

   * - Domain
     - Tick Counter
     - Example Sites
   * - Mailbox
     - Per-enqueue/dequeue
     - Enqueue, dequeue, overflow
   * - Transport
     - Per-send/recv
     - try_send, frame encode/decode
   * - Scheduler
     - Per-dispatch
     - dispatch, steal, timer fire
   * - Allocator
     - Per-alloc/free
     - allocate, deallocate, compact
   * - Storage
     - Per-I/O
     - Read, write, flush
   * - Timer
     - Per-timer-tick
     - Timer insert, cancel, fire
   * - Gossip
     - Per-protocol-message
     - Ping, ack, indirect probe
   * - Config
     - Per-parse
     - TOML parse, binary load
   * - Actor
     - Per-lifecycle
     - Spawn, stop, restart
   * - Metrics
     - Per-event
     - Ring buffer write, scrape
   * - Tracing
     - Per-span
     - Span start, end, export
   * - Logging
     - Per-log-entry
     - Ring buffer write, drain
   * - Network
     - Per-connection
     - Connect, accept, close
   * - Supervision
     - Per-restart
     - on_child_failure, restart_child

Fault Actions (5 types)
-----------------------

.. list-table::
   :header-rows: 1

   * - Action
     - Effect
     - Use Case
   * - ``Fail``
     - Return an error code
     - Simulate component failure
   * - ``Drop``
     - Silently discard the operation
     - Simulate message loss
   * - ``Delay``
     - Add artificial latency
     - Simulate network/cpu contention
   * - ``Corrupt``
     - Flip bits in the payload
     - Simulate memory/transmission errors
   * - ``Panic``
     - Trigger a crash/assert
     - Test crash recovery

Building a Fault Schedule
-------------------------

.. code-block:: cpp

   #include <hpactor/fault/fault_schedule.hpp>

   auto schedule = FaultSchedule::builder()
       .at("hpactor.mailbox.enqueue", FaultAction::Drop)
           .on_tick(100)                    // Drop the 100th enqueue
       .at("hpactor.transport.try_send", FaultAction::Delay)
           .with_delay_ms(500)
           .on_tick(50)                     // Delay the 50th send by 500ms
       .at("hpactor.scheduler.*", FaultAction::Fail)
           .on_tick(25)                     // Fail any scheduler op at tick 25
       .build();

   system.fault_controller().load_schedule(std::move(schedule));
   system.fault_controller().enable();

Seed-Replayable Determinism
---------------------------

Fault schedules are deterministic — the same seed produces the same
sequence of faults:

.. code-block:: cpp

   // Record a fault run:
   uint64_t seed = 0xDEADBEEF;
   auto schedule = FaultSchedule::generate(seed, config);
   system.fault_controller().load_schedule(schedule);

   // Replay: same seed → same schedule → same failures
   auto replay_schedule = FaultSchedule::generate(seed, config);
   assert(schedule == replay_schedule);

This makes chaos experiments reproducible — ideal for CI pipelines and
regression testing.

CLI Commands
------------

.. code-block:: text

   /fault status           # Show controller state (enabled/disabled, schedule info)
   /fault list             # List all registered fault points and active injections
   /fault clear            # Clear all active schedules

Compile-Time Disable
--------------------

.. code-block:: bash

   cmake -DENABLE_FAULT_INJECTION=OFF ..

When disabled, all ``FAULT_INJECT()`` macros become no-ops and the
``fault::`` namespace compiles to empty stubs — zero runtime overhead.

Chaos Experiment Design
-----------------------

A well-designed chaos experiment follows this template:

1. **Hypothesis**: "If the mailbox drops 10% of messages, the Dead-Letter
   Queue should capture them and the system should continue processing."

2. **Schedule Design**:

   .. code-block:: cpp

      auto schedule = FaultSchedule::builder()
          .at("hpactor.mailbox.enqueue", FaultAction::Drop)
              .with_probability(0.1)    // Drop 10% of enqueues
              .build();

3. **Observation**: Monitor ``hpactor_dlq_records_total`` and
   ``hpactor_delivery_failures_total`` during the experiment.

4. **Validation**: After the experiment, verify:
   - DLQ records match dropped messages.
   - No unexpected actor crashes.
   - System throughput degraded proportionally (not catastrophically).

5. **Cleanup**: ``/fault clear`` and confirm ``/fault status`` shows
   disabled.

CI Integration
--------------

Run chaos tests in CI with known seeds:

.. code-block:: yaml

   # GitHub Actions example
   - name: Chaos Tests (Seed 0xDEAD)
     run: |
       ./build/tests/system/test_chaos_mailbox --gtest_filter="*Seed0xDEAD*"

   - name: Chaos Tests (Seed 0xBEEF)
     run: |
       ./build/tests/system/test_chaos_mailbox --gtest_filter="*Seed0xBEEF*"
