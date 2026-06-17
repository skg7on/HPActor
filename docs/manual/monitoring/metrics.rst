.. _monitoring-metrics:

Metrics
=======

HPActor exposes Prometheus-compatible metrics through a lock-free
ring buffer instrumentation system. This chapter covers configuring,
exposing, and consuming actor metrics.

Architecture
------------

.. code-block:: text

   Actor/Mailbox/Scheduler ──► MpscRingBuffer ──► MetricsActor
                                                      │
                                            /metrics endpoint
                                            (Prometheus scrape)

Metrics flow through a three-stage pipeline:

1. **Emission** — components write ``MetricEvent`` structs (32 bytes) to
   a CAS-based lock-free ring buffer.
2. **Aggregation** — the :cpp:class:`MetricsActor` drains the ring buffer
   on each ``/metrics`` scrape and updates counters, gauges, and histograms.
3. **Exposition** — :cpp:class:`OpenMetricsFormatter` renders the metrics
   in ``text/plain; version=1.0.0`` Prometheus format.

Metric Types
------------

.. list-table::
   :header-rows: 1

   * - Type
     - Description
     - Example
   * - **Counter**
     - Monotonically increasing value
     - ``hpactor_messages_sent_total``
   * - **Gauge**
     - Point-in-time value
     - ``hpactor_mailbox_depth_current``
   * - **Histogram**
     - Distribution with buckets
     - ``hpactor_processing_latency_seconds``

Event Types
-----------

The 32-byte ``MetricEvent`` encodes 10 event types:

.. list-table::
   :header-rows: 1

   * - Event
     - Description
     - Labels
   * - ``kMailboxEnqueue``
     - Message enqueued into mailbox
     - actor_id, actor_type
   * - ``kMailboxDequeue``
     - Message dequeued for processing
     - actor_id, actor_type
   * - ``kProcessingLatency``
     - Message processing duration
     - actor_id, actor_type, duration_us
   * - ``kActorSpawned``
     - Actor created
     - actor_id, actor_type
   * - ``kActorTerminated``
     - Actor destroyed
     - actor_id, actor_type, reason
   * - ``kSchedulerDispatch``
     - Actor dispatched to worker
     - worker_id
   * - ``kSchedulerSteal``
     - Work stolen by a worker
     - worker_id, victim_id
   * - ``kSupervisionRestart``
     - Actor restarted by supervisor
     - actor_id, actor_type
   * - ``kAllocation``
     - Memory allocated
     - region, size_bytes
   * - ``kDeallocation``
     - Memory freed
     - region, size_bytes
   * - ``kDeliveryFailure``
     - Message delivery failed
     - actor_id, reason

Configuration
-------------

Enable and configure metrics via TOML:

.. code-block:: toml

   [system.metrics]
   enabled = true
   metrics_path = "/metrics"          # HTTP endpoint path
   ring_buffer_capacity = 65536       # Event buffer size

Or via CMake:

.. code-block:: bash

   cmake -DENABLE_ACTOR_METRICS=ON ..   # default ON
   cmake -DENABLE_ACTOR_METRICS=OFF ..  # disable at compile time

Prometheus Scrape Configuration
-------------------------------

If using the :cpp:class:`HTTPGatewayActor`, expose ``/metrics``:

.. code-block:: toml

   [[actors]]
   name = "http-gateway"
   behavior = "http_gateway"
   args = { listen_port = 8080 }

Then configure Prometheus:

.. code-block:: yaml

   # prometheus.yml
   scrape_configs:
     - job_name: 'hpactor'
       static_configs:
         - targets: ['localhost:8080']
       metrics_path: '/metrics'
       scrape_interval: 15s

Available Metrics
-----------------

.. list-table::
   :header-rows: 1

   * - Metric Name
     - Type
     - Description
   * - ``hpactor_actors_spawned_total``
     - Counter
     - Lifetime actor spawn count
   * - ``hpactor_actors_terminated_total``
     - Counter
     - Lifetime actor termination count
   * - ``hpactor_actors_active``
     - Gauge
     - Currently active actors
   * - ``hpactor_messages_enqueued_total``
     - Counter
     - Messages enqueued across all mailboxes
   * - ``hpactor_messages_dequeued_total``
     - Counter
     - Messages dequeued for processing
   * - ``hpactor_mailbox_depth``
     - Gauge
     - Current mailbox depth (per actor)
   * - ``hpactor_processing_latency_seconds``
     - Histogram
     - Message processing latency distribution
   * - ``hpactor_scheduler_dispatches_total``
     - Counter
     - Scheduler dispatches
   * - ``hpactor_scheduler_steals_total``
     - Counter
     - Work steals between workers
   * - ``hpactor_supervision_restarts_total``
     - Counter
     - Actor restarts by supervisors
   * - ``hpactor_memory_allocations_total``
     - Counter
     - Memory allocations (per region)
   * - ``hpactor_memory_bytes_allocated``
     - Gauge
     - Currently allocated bytes (per region)
   * - ``hpactor_delivery_failures_total``
     - Counter
     - Failed message deliveries
   * - ``hpactor_dlq_records_total``
     - Gauge
     - Dead-letter queue records

Programmatic Access
-------------------

Query metrics from code (for integration tests, health checks, etc.):

.. code-block:: cpp

   // Scrape metrics via message passing to the MetricsActor
   MetricsRequest req;
   auto handle = context()->ask<MetricsResponse>(system.metrics_actor(), req);
   auto response = handle.get();
   std::string prometheus_text = response->body();

Custom Metrics
--------------

Add application-specific counters:

.. code-block:: cpp

   class MyInstrumentedActor : public EventBasedActor {
       std::atomic<int64_t> orders_processed_{0};

       Behavior make_behavior() override {
           return Behavior::make()
               .on<Order>([this](const Order& order) {
                   process(order);
                   orders_processed_.fetch_add(1);
               });
       }

   public:
       int64_t orders_processed() const { return orders_processed_.load(); }
   };

Custom metrics are exposed through the ``to_metadata()`` / ``serialize_state()``
virtual interface and will appear in ``/actor <id> show`` CLI output.

For dashboard and visualization guidance, see
:doc:`/sre-integration/prometheus-grafana`.
