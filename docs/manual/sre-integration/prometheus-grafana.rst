.. _sre-integration-prometheus-grafana:

Prometheus & Grafana
====================

This guide covers integrating HPActor metrics with Prometheus for
collection and Grafana for visualization.

Prometheus Integration
----------------------

HPActor exposes a ``/metrics`` endpoint in Prometheus OpenMetrics format.
Any actor system with the HTTP gateway or metrics actor enabled can be
scraped.

**Prerequisites:**
- Metrics enabled: CMake ``ENABLE_ACTOR_METRICS=ON``, TOML ``[system.metrics] enabled = true``.
- HTTP gateway running on a known port, or a standalone metrics endpoint.

Prometheus Configuration
~~~~~~~~~~~~~~~~~~~~~~~~

.. code-block:: yaml

   # prometheus.yml
   global:
     scrape_interval: 15s
     evaluation_interval: 15s

   scrape_configs:
     - job_name: 'hpactor'
       static_configs:
         - targets:
             - 'hpactor-node-1:8080'
             - 'hpactor-node-2:8080'
             - 'hpactor-node-3:8080'
       metrics_path: '/metrics'
       scrape_interval: 10s     # More frequent for actor systems

   # If using service discovery:
     - job_name: 'hpactor-gossip'
       dns_sd_configs:
         - names:
             - 'hpactor.internal'
           type: 'A'
           port: 8080

Key Metrics to Watch
~~~~~~~~~~~~~~~~~~~~

.. list-table::
   :header-rows: 1

   * - Metric
     - Alert Threshold
     - What It Means
   * - ``hpactor_mailbox_depth``
     - > 80% of capacity
     - Actor falling behind; may overflow
   * - ``hpactor_processing_latency_seconds``
     - p99 > 500ms
     - Slow message processing; check handler logic
   * - ``hpactor_supervision_restarts_total``
     - rate > 0.1/s
     - Actor crash loop; supervisor may quarantine
   * - ``hpactor_delivery_failures_total``
     - rate > 0/s
     - Messages failing to deliver; check DLQ
   * - ``hpactor_dlq_records_total``
     - growing
     - DLQ accumulating; investigate undeliverable messages
   * - ``hpactor_actors_active``
     - sudden drop
     - Mass actor termination; possible crash cascade
   * - ``hpactor_memory_bytes_allocated``
     - > 80% of limit
     - Memory pressure; may trigger admission control

Grafana Dashboard
-----------------

Import or build a dashboard to visualize HPActor metrics.

Sample Dashboard Panels
~~~~~~~~~~~~~~~~~~~~~~~

**Actor Throughput** (Stat):

.. code-block:: promql

   rate(hpactor_messages_enqueued_total[1m])

**Mailbox Depth by Actor** (Time Series):

.. code-block:: promql

   hpactor_mailbox_depth

**Processing Latency Heatmap** (Heatmap):

.. code-block:: promql

   rate(hpactor_processing_latency_seconds_bucket[5m])

**Scheduler Work Distribution** (Time Series, stacked):

.. code-block:: promql

   rate(hpactor_scheduler_dispatches_total[1m])
   rate(hpactor_scheduler_steals_total[1m])

**Supervision Restarts** (Stat with sparkline):

.. code-block:: promql

   rate(hpactor_supervision_restarts_total[5m])

**Active Actors** (Gauge):

.. code-block:: promql

   hpactor_actors_active

**Dead-Letter Queue Growth** (Time Series):

.. code-block:: promql

   hpactor_dlq_records_total

**Memory by Region** (Time Series, stacked):

.. code-block:: promql

   hpactor_memory_bytes_allocated

Dashboard JSON Template
~~~~~~~~~~~~~~~~~~~~~~~

A starter dashboard JSON can be generated from the HPActor test suite.
See ``docs/manual/dashboards/`` for importable Grafana dashboard JSON
files. To generate a dashboard from your own actor types:

.. code-block:: bash

   # Dump metric metadata to understand what's available:
   curl http://localhost:8080/metrics | grep "# HELP"

Recording Rules
~~~~~~~~~~~~~~~

Pre-compute expensive or frequently-needed metrics:

.. code-block:: yaml

   # prometheus_rules.yml
   groups:
     - name: hpactor
       rules:
         - record: hpactor:message_rate_1m
           expr: rate(hpactor_messages_enqueued_total[1m])

         - record: hpactor:error_rate_1m
           expr: rate(hpactor_delivery_failures_total[1m])

         - record: hpactor:actor_restart_rate_5m
           expr: rate(hpactor_supervision_restarts_total[5m])

Grafana Provisioning
--------------------

For automated dashboard deployment, use Grafana provisioning:

.. code-block:: yaml

   # grafana/provisioning/dashboards/hpactor.yml
   apiVersion: 1
   providers:
     - name: 'HPActor'
       orgId: 1
       folder: 'HPActor'
       type: file
       options:
         path: /etc/grafana/dashboards/hpactor/

Docker Compose Example
----------------------

.. code-block:: yaml

   # docker-compose.yml
   version: '3'
   services:
     hpactor-app:
       image: my-hpactor-app:latest
       ports:
         - "8080:8080"
         - "9090:9090"

     prometheus:
       image: prom/prometheus:latest
       volumes:
         - ./prometheus.yml:/etc/prometheus/prometheus.yml
       ports:
         - "9090:9090"

     grafana:
       image: grafana/grafana:latest
       volumes:
         - ./grafana/dashboards:/etc/grafana/dashboards
         - ./grafana/provisioning:/etc/grafana/provisioning
       ports:
         - "3000:3000"
       environment:
         - GF_SECURITY_ADMIN_PASSWORD=admin
