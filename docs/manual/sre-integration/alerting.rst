.. _sre-integration-alerting:

Alerting Rules
==============

This chapter provides pre-built Prometheus alerting rules for HPActor
systems. These rules detect actor failures, resource pressure, and
degradation.

Alert Severity Levels
---------------------

.. list-table::
   :header-rows: 1

   * - Severity
     - Response
     - Example
   * - **Critical**
     - Immediate paging (PagerDuty, Opsgenie)
     - Mass actor crash, system shutdown
   * - **Warning**
     - On-call investigation during business hours
     - Mailbox approaching capacity, elevated latency
   * - **Info**
     - Dashboard visibility, no alert
     - Slow growth in DLQ, occasional restart

AlertManager Configuration
--------------------------

.. code-block:: yaml

   # alertmanager.yml
   route:
     receiver: 'default'
     routes:
       - match:
           severity: critical
         receiver: 'pagerduty-critical'
         repeat_interval: 5m
       - match:
           severity: warning
         receiver: 'slack-warnings'
         repeat_interval: 30m

   receivers:
     - name: 'pagerduty-critical'
       pagerduty_configs:
         - routing_key: 'YOUR_PD_ROUTING_KEY'

     - name: 'slack-warnings'
       slack_configs:
         - api_url: 'YOUR_SLACK_WEBHOOK_URL'
           channel: '#hpactor-alerts'

Prometheus Alert Rules
----------------------

.. code-block:: yaml

   # hpactor_alerts.yml
   groups:
     - name: hpactor_critical
       rules:
         # ---- Actor Failures ----
         - alert: HighActorRestartRate
           expr: rate(hpactor_supervision_restarts_total[5m]) > 0.1
           for: 2m
           labels:
             severity: critical
           annotations:
             summary: "High actor restart rate ({{ $value }}/s)"
             description: "Actor {{ $labels.actor_type }} is restarting at {{ $value }}/s. Check for crash loops."

         - alert: MassActorTermination
           expr: (hpactor_actors_active - hpactor_actors_active offset 1m) < -10
           for: 1m
           labels:
             severity: critical
           annotations:
             summary: "Mass actor termination detected"
             description: "{{ $value }} actors terminated in the last minute. Investigate immediately."

         # ---- Delivery Failures ----
         - alert: DeliveryFailureRate
           expr: rate(hpactor_delivery_failures_total[5m]) > 0
           for: 5m
           labels:
             severity: warning
           annotations:
             summary: "Message delivery failures detected"
             description: "Delivery failure rate: {{ $value }}/s. Check DLQ."

         # ---- Dead Letter Queue ----
         - alert: DLQGrowing
           expr: deriv(hpactor_dlq_records_total[1h]) > 10
           for: 10m
           labels:
             severity: warning
           annotations:
             summary: "Dead-letter queue is growing"
             description: "DLQ growing at {{ $value }} records/hour. Investigate undeliverable messages."

         - alert: DLQCritical
           expr: hpactor_dlq_records_total > 10000
           labels:
             severity: critical
           annotations:
             summary: "Dead-letter queue exceeded 10,000 records"
             description: "DLQ has {{ $value }} records. Replay or clear immediately."

     - name: hpactor_resource
       rules:
         # ---- Mailbox Pressure ----
         - alert: MailboxNearCapacity
           expr: (hpactor_mailbox_depth / hpactor_mailbox_capacity) > 0.8
           for: 5m
           labels:
             severity: warning
           annotations:
             summary: "Mailbox near capacity ({{ $value | humanizePercentage }})"
             description: "Actor {{ $labels.actor_id }} mailbox at {{ $value | humanizePercentage }} capacity."

         - alert: MailboxFull
           expr: (hpactor_mailbox_depth / hpactor_mailbox_capacity) >= 1
           for: 1m
           labels:
             severity: critical
           annotations:
             summary: "Mailbox full for {{ $labels.actor_id }}"
             description: "Actor {{ $labels.actor_id }} mailbox is full. Messages being dropped or sent to DLQ."

         # ---- Latency ----
         - alert: HighProcessingLatency
           expr: histogram_quantile(0.99, rate(hpactor_processing_latency_seconds_bucket[5m])) > 0.5
           for: 5m
           labels:
             severity: warning
           annotations:
             summary: "High p99 processing latency ({{ $value }}s)"
             description: "p99 message processing latency is {{ $value }}s for {{ $labels.actor_type }}."

         # ---- Memory ----
         - alert: HighMemoryUsage
           expr: hpactor_memory_bytes_allocated > (hpactor_memory_limit_bytes * 0.85)
           for: 5m
           labels:
             severity: warning
           annotations:
             summary: "Memory usage above 85% ({{ $value | humanize }}B)"
             description: "Region {{ $labels.region }} using {{ $value | humanize }}B."

         # ---- Scheduler ----
         - alert: SchedulerImbalance
           expr: |
             (
               max(rate(hpactor_scheduler_dispatches_total[5m])) -
               min(rate(hpactor_scheduler_dispatches_total[5m]))
             ) > 50
           for: 5m
           labels:
             severity: warning
           annotations:
             summary: "Scheduler worker imbalance detected"
             description: "Work distribution is skewed across workers."

         # ---- System ----
         - alert: HPActorDown
           expr: up{job="hpactor"} == 0
           for: 1m
           labels:
             severity: critical
           annotations:
             summary: "HPActor instance {{ $labels.instance }} is down"
             description: "The metrics endpoint is unreachable."

Runbooks
--------

For each alert, maintain a runbook:

**HighActorRestartRate:**
  1. SSH to the affected node.
  2. Check CLI ``/actor list`` for the crashing actor.
  3. Check ``/dlq list`` for messages sent to the actor.
  4. Check logs: ``/actor <id> inspect`` or ``journalctl -u hpactor``.
  5. If restart loop persists, check quarantine status with ``/fault status``.
  6. Escalate to application team if actor logic is buggy.

**MailboxFull:**
  1. Identify the bottleneck actor.
  2. Check processing latency — is the handler slow?
  3. Check downstream dependencies — is the actor blocked on an ask/response?
  4. Increase mailbox capacity (if legitimate backlog) or add more instances.
  5. Consider enabling DLQ overflow as a safety valve.

**DLQCritical:**
  1. List DLQ records: ``/dlq list``.
  2. Identify common sender/receiver patterns.
  3. Export for offline analysis: ``/dlq export --format json > dlq_dump.json``.
  4. Replay valid records: ``/dlq replay <index>``.
  5. Fix the root cause (routing, handler bug, capacity issue).

**HPActorDown:**
  1. Check if the node is reachable.
  2. SSH and check ``systemctl status hpactor``.
  3. Check ``journalctl -u hpactor --since "5 minutes ago"`` for crash logs.
  4. Check for OOM: ``journalctl -u hpactor | grep -i "out of memory"``.
  5. If daemon mode, check ``/var/run/hpactor/hpactor.pid``.
  6. Restart if safe: ``systemctl restart hpactor``.
