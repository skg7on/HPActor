.. _best-practices-performance:

Performance Tuning
==================

This chapter covers tuning HPActor for throughput and latency, from
scheduler configuration to memory allocator settings.

Scheduler Configuration
-----------------------

.. code-block:: toml

   [system]
   scheduler_threads = 8              # Number of worker threads
   scheduler_policy = "work_stealing" # Work-stealing algorithm

Guidelines for ``scheduler_threads``:
- **I/O-bound workloads**: 1–2× CPU cores.
- **CPU-bound workloads**: 1× CPU cores (dedicated threads for compute).
- **Mixed**: Start at CPU cores and profile.

The :cpp:class:`HybridScheduler` uses A2WS (Adaptive Two-Level Work
Stealing) — workers prefer their local queue and steal from others only
when idle.

Worker Affinity
---------------

Pin workers to specific CPUs for cache locality:

.. code-block:: toml

   [[dispatchers]]
   name = "cpu-dispatcher"
   type = "dense_compute"
   thread_count = 4
   cpu_affinity = [0, 1, 2, 3]      # Pin to cores 0-3

Mailbox Tuning
--------------

.. code-block:: toml

   [system.mailbox]
   priority_aware = true
   priority_levels = 4               # 0 = highest, 3 = lowest

Per-actor mailbox configuration:

.. code-block:: toml

   [[actors]]
   name = "high-throughput-worker"
   behavior = "worker"
   mailbox_capacity = 65536
   mailbox_overflow = "drop_oldest"  # Shed load when full
   priority_level = 1

Tuning guidelines:
- **Capacity**: Start at 1024, raise if ``hpactor_mailbox_depth`` stays near capacity.
- **Overflow policy**: ``drop_oldest`` for real-time systems, ``dlq`` for no-loss.
- **Priority lanes**: Reserve lane 0 for system/control, lane 3 for best-effort.

Memory Allocator
----------------

HPActor's two-tier slab allocator is tuned by default. Adjust for
workload-specific patterns:

.. code-block:: toml

   [system.memory]
   segment_size_mb = 2               # Size of each mmap segment
   max_regions = 16                  # Max memory regions

   [system.memory.regions.actor]
   hard_limit_mb = 512               # Cap actor memory at 512MB

   [system.memory.regions.message]
   hard_limit_mb = 256               # Cap message memory at 256MB

When ``ENABLE_MEMORY_DEBUG=ON``:
- Memory poisoning detects use-after-free.
- Canary verification detects buffer overflows.
- Guard pages catch out-of-bounds access on hibernated actors.

.. note::

   Memory debugging has ~10-20% overhead. Enable it in CI and staging
   but disable in production unless investigating a memory issue.

Coroutine Tuning
----------------

.. code-block:: toml

   [system.coroutine]
   frame_pool_size = 4096            # Pre-allocated coroutine frames

The coroutine frame pool eliminates per-suspension allocations. Tune
``frame_pool_size`` based on peak concurrency — each in-flight actor
suspension uses one frame.

Message Batching
----------------

For high-throughput scenarios, batch messages to reduce per-message
overhead:

.. code-block:: cpp

   // Instead of sending 1000 individual messages:
   for (auto& item : items) {
       context()->send(worker, ProcessItem{item});  // 1000 sends
   }

   // Batch them:
   BatchRequest batch;
   for (auto& item : items) {
       *batch.add_items() = item;
   }
   context()->send(worker, batch);  // 1 send

Each message incurs:
- Enqueue (CAS on ring buffer).
- Dequeue (load-acquire).
- Dispatch (scheduler queue operations).
- Deserialization (for protobuf messages).

Batching amortizes this overhead across many items.

Measuring Performance
---------------------

**Throughput** — messages processed per second:

.. code-block:: promql

   rate(hpactor_messages_dequeued_total[1m])

**Latency** — p50/p95/p99 processing time:

.. code-block:: promql

   histogram_quantile(0.99, rate(hpactor_processing_latency_seconds_bucket[5m]))

**Scheduler balance** — work distribution across workers:

.. code-block:: promql

   rate(hpactor_scheduler_dispatches_total[1m])

   # Check for imbalance:
   stddev(rate(hpactor_scheduler_dispatches_total[5m]))

**Mailbox pressure** — percentage of capacity:

.. code-block:: promql

   hpactor_mailbox_depth / hpactor_mailbox_capacity

Performance Checklist
---------------------

- [ ] Scheduler threads match CPU topology (1-2× cores for I/O, 1× for CPU).
- [ ] Mailbox capacity tuned per actor (not one-size-fits-all).
- [ ] Overflow policy matches message importance.
- [ ] Priority lanes separate control from data messages.
- [ ] Hot-path messages batched where possible.
- [ ] Memory limits set per region.
- [ ] Coroutine frame pool sized for peak concurrency.
- [ ] Profiling confirms no blocking calls on cooperative scheduler.
- [ ] ``hpactor_processing_latency_seconds`` p99 < target SLA.
- [ ] DLQ growth is monitored and bounded.
