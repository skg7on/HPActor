.. _monitoring-health:

Health & Readiness
==================

HPActor provides health-check infrastructure for orchestrators,
load balancers, and monitoring systems.

HealthHttpServer
----------------

The :cpp:class:`HealthHttpServer` exposes HTTP endpoints for liveness
and readiness probes:

.. code-block:: text

   GET /healthz     → 200 OK (process alive)
   GET /readyz      → 200 OK (ready for traffic) or 503 (not ready)

Configuration via TOML:

.. code-block:: toml

   [system.process]
   health_port = 9090
   health_bind = "127.0.0.1"   # Only listen on localhost

.. code-block:: text

   $ curl http://localhost:9090/healthz
   OK
   $ curl http://localhost:9090/readyz
   OK

Liveness vs. Readiness
----------------------

.. list-table::
   :header-rows: 1

   * - Probe
     - Endpoint
     - Meaning
     - When 503
   * - **Liveness**
     - ``/healthz``
     - The process is alive and not deadlocked
     - Fatal crash, deadlock
   * - **Readiness**
     - ``/readyz``
     - The system is ready to accept traffic
     - During startup, during drain, after fatal subsystem failure

Readiness transitions:
- **Not ready** — during ``ActorSystem`` construction and topology bootstrap.
- **Ready** — after ``SystemInitTag`` is broadcast (all actors spawned).
- **Not ready** — during graceful shutdown (drain phase).

Kubernetes Pod Configuration
----------------------------

.. code-block:: yaml

   apiVersion: v1
   kind: Pod
   spec:
     containers:
       - name: hpactor-app
         image: my-app:latest
         ports:
           - containerPort: 9090
         livenessProbe:
           httpGet:
             path: /healthz
             port: 9090
           initialDelaySeconds: 5
           periodSeconds: 10
         readinessProbe:
           httpGet:
             path: /readyz
             port: 9090
           initialDelaySeconds: 3
           periodSeconds: 5

systemd Watchdog
----------------

In daemon mode, HPActor supports systemd's ``Type=notify`` with watchdog:

.. code-block:: ini

   # /etc/systemd/system/hpactor.service
   [Service]
   Type=notify
   WatchdogSec=10s
   NotifyAccess=main

The :cpp:class:`WatchdogActor` periodically calls ``sd_notify(WATCHDOG=1)``
to signal liveliness. If the watchdog timer expires without a notification,
systemd restarts the service.

For daemon deployment, see :doc:`/operations/daemon-mode`.

Custom Health Checks
--------------------

Implement application-specific health checks:

.. code-block:: cpp

   class MyHealthChecker : public IHealthCheck {
   public:
       HealthStatus check() override {
           if (db_connection_alive()) {
               return HealthStatus::healthy();
           }
           return HealthStatus::unhealthy("database connection lost");
       }
   };

   // Register with the health server
   system.health_server().add_check(std::make_unique<MyHealthChecker>());

Health checks contribute to the overall readiness status — if any check
is unhealthy, ``/readyz`` returns 503.
