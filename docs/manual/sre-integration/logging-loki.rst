.. _sre-integration-logging-loki:

Log Aggregation with Loki
==========================

This guide covers integrating HPActor structured logging with Grafana
Loki for centralized log aggregation and correlation with metrics and
traces.

Integration Strategy
--------------------

HPActor's :cpp:class:`JsonLogFormatter` produces structured JSON log
entries that include W3C trace context. This enables correlation between
logs, traces, and metrics in Grafana.

Architecture:

.. code-block:: text

   HPActor ──► JsonLogFormatter ──► File/RotatingFile ──► Promtail ──► Loki
             │                                                                │
             └── TraceId, SpanId in each log line ────────────────────────────┘
                                                              Grafana
                                                      (Logs + Metrics + Traces)

Configuring JSON Log Format
---------------------------

Enable JSON output for machine parsing:

.. code-block:: toml

   [system.log]
   level = "info"
   sinks = ["rotating_file"]

   [system.log.rotating_file]
   path = "/var/log/hpactor/hpactor.log"
   max_size_mb = 100
   max_files = 10

   # Configure JSON format (default is text for development)
   [system.log.formatter]
   type = "json"

Example JSON log line:

.. code-block:: json

   {
     "timestamp": "2026-06-17T10:15:30.123Z",
     "level": "info",
     "category": "actor.lifecycle",
     "message": "Actor worker-1 spawned",
     "actor_id": "actor:42",
     "actor_type": "worker",
     "trace_id": "0a1b2c3d4e5f6789abcdef0123456789",
     "span_id": "1234567890abcdef"
   }

Promtail Configuration
----------------------

.. code-block:: yaml

   # promtail.yaml
   server:
     http_listen_port: 9080

   clients:
     - url: http://loki:3100/loki/api/v1/push

   scrape_configs:
     - job_name: hpactor
       static_configs:
         - targets:
             - localhost
           labels:
             job: hpactor
             node: ${HOSTNAME}
             __path__: /var/log/hpactor/hpactor.log
       pipeline_stages:
         - json:
             expressions:
               timestamp: timestamp
               level: level
               category: category
               message: message
               actor_id: actor_id
               trace_id: trace_id
               span_id: span_id

Loki LogQL Queries
------------------

.. code-block:: text

   # All error logs
   {job="hpactor"} |= "error"

   # Delivery failures for a specific actor
   {job="hpactor"} | json | category="delivery.failure" | actor_id="actor:42"

   # Mailbox pressure warnings
   {job="hpactor"} | json | category="mailbox.pressure"

   # Logs correlated with a specific trace
   {job="hpactor"} | json | trace_id="0a1b2c3d4e5f6789abcdef0123456789"

   # Recent supervision restarts
   {job="hpactor"} | json | category="supervision.restart" | line_format "{{.actor_id}} restarted: {{.message}}"

Grafana Correlation
-------------------

When Loki, Tempo/Jaeger, and Prometheus are all configured as Grafana
data sources, you can:

- **Logs → Traces**: Click a ``trace_id`` in a Loki log entry to jump
  to the corresponding trace in Tempo/Jaeger.
- **Traces → Logs**: View logs associated with a span.
- **Metrics → Logs**: Drill from a Prometheus alert (e.g., high mailbox
  depth) to the corresponding actor's logs.

This requires the trace context (``trace_id``, ``span_id``) in each log
line — which the JSON formatter includes automatically when
``ENABLE_ACTOR_TRACING=ON``.

ELK Stack Alternative
---------------------

For teams using Elasticsearch, the JSON log format is equally compatible
with Filebeat → Elasticsearch pipelines:

.. code-block:: yaml

   # filebeat.yml
   filebeat.inputs:
     - type: log
       enabled: true
       paths:
         - /var/log/hpactor/hpactor.log
       json.keys_under_root: true
       json.add_error_key: true

   output.elasticsearch:
     hosts: ["elasticsearch:9200"]

Log Retention & Rotation
------------------------

HPActor's :cpp:class:`RotatingFileSink` handles local log rotation.
For long-term retention, rely on Loki or Elasticsearch — configure
appropriate retention policies there, not in the application.

.. code-block:: toml

   [system.log.rotating_file]
   path = "/var/log/hpactor/hpactor.log"
   max_size_mb = 100
   max_files = 10   # Keep ~1GB of local logs

For daemon mode with syslog, journald handles rotation automatically:

.. code-block:: bash

   # View daemon logs
   sudo journalctl -u hpactor -f --output=json
