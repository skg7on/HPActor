.. _best-practices-deployment:

Production Deployment
=====================

This chapter covers deploying HPActor applications to production,
including build optimization, configuration management, and
infrastructure integration.

Build for Production
--------------------

.. code-block:: bash

   cmake -S . -B build -GNinja \
       -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_EXAMPLES=OFF \
       -DENABLE_APPS=OFF \
       -DENABLE_COVERAGE=OFF \
       -DENABLE_MEMORY_DEBUG=OFF \
       -DCMAKE_CXX_FLAGS="-O3 -march=native -flto"

   ninja -C build
   strip build/apps/your_app

Optimization flags:
- ``-O3`` — aggressive optimization.
- ``-march=native`` — CPU-specific instructions when building on target hardware.
- ``-flto`` — link-time optimization.
- ``strip`` — remove debug symbols.

AOT Topology Compilation
------------------------

Pre-compile TOML to binary for zero-parse startup:

.. code-block:: bash

   ./build/tools/toml-compiler/hpactor_toml_compiler \
       --input config/topology.toml \
       --output config/topology.bin

   # Verify:
   ./build/tools/toml-compiler/hpactor_toml_compiler \
       --verify config/topology.bin

Load the binary at runtime:

.. code-block:: cpp

   system.load_topology("config/topology.bin");  // mmap + zero-parse

Configuration Management
------------------------

**Per-environment configs:**

.. code-block:: text

   config/
   ├── base.toml               # Common settings
   ├── development.toml        # Dev overrides
   ├── staging.toml            # Staging overrides
   └── production.toml         # Production overrides

Use TOML template inheritance:

.. code-block:: toml

   # production.toml
   [[imports]]
   path = "base.toml"

   [system]
   scheduler_threads = 16       # Override base

   [system.metrics]
   enabled = true               # Metrics on in prod

**Secrets management:**
- Never put secrets in TOML files.
- Inject secrets via environment variables or a secrets manager such as Vault or AWS Secrets Manager.
- Read secrets at startup in ``main()`` before ``ActorSystem`` construction.

Daemon Deployment
-----------------

For server deployments, run as a daemon with systemd (see
:doc:`/operations/daemon-mode` for the full unit file):

.. code-block:: bash

   sudo systemctl enable hpactor
   sudo systemctl start hpactor

Key systemd hardening options:

.. code-block:: ini

   [Service]
   # Prevent privilege escalation
   NoNewPrivileges=yes

   # Isolate filesystem
   ProtectSystem=strict
   ProtectHome=yes
   ReadWritePaths=/var/lib/hpactor /var/log/hpactor

   # Restrict network (if only local IPC needed)
   # PrivateNetwork=yes

   # Memory limits
   MemoryMax=2G

Containerization
----------------

.. code-block:: dockerfile

   # Dockerfile
   FROM ubuntu:22.04 AS builder
   RUN apt-get update && apt-get install -y \
       cmake ninja-build g++ libssl-dev protobuf-compiler libprotobuf-dev
   COPY . /src
   WORKDIR /src
   RUN cmake -S . -B build -GNinja \
       -DCMAKE_BUILD_TYPE=Release \
       -DENABLE_EXAMPLES=OFF \
       -DENABLE_APPS=OFF
   RUN ninja -C build

   FROM ubuntu:22.04
   RUN apt-get update && apt-get install -y \
       libssl3 libprotobuf32
   COPY --from=builder /src/build/apps/your_app /usr/local/bin/
   COPY --from=builder /src/config/topology.bin /etc/hpactor/
   EXPOSE 8080 9090
   USER hpactor
   CMD ["/usr/local/bin/your_app", "--topology", "/etc/hpactor/topology.bin"]

Health-Check Aware Load Balancers
----------------------------------

If running behind Nginx or HAProxy, configure health checks:

.. code-block:: nginx

   upstream hpactor_backend {
       server 10.0.1.1:8080 max_fails=3 fail_timeout=30s;
       server 10.0.1.2:8080 max_fails=3 fail_timeout=30s;

       health_check uri=/healthz interval=5s;
       health_check uri=/readyz interval=5s;  # Remove from pool if not ready
   }

Graceful Rollout
----------------

For rolling updates (future, with cluster control plane):

1. Send ``SIGTERM`` to the old process → triggers drain.
2. Wait for drain to complete (monitor ``/readyz`` → 503).
3. Start the new process.
4. Wait for ``/readyz`` → 200.
5. Route traffic to the new process.

Currently, HPActor supports graceful shutdown of individual nodes but
not coordinated rolling upgrades across a cluster (see
:doc:`/limitations`).

Monitoring in Production
------------------------

Must-have monitoring for production:

.. list-table::
   :header-rows: 1

   * - Signal
     - Tool
     - Alert
   * - Metrics
     - Prometheus + Grafana
     - Mailbox pressure, restart rate, DLQ growth
   * - Logs
     - Loki or ELK
     - Error rate spike, delivery failures
   * - Traces
     - Jaeger or Tempo
     - p99 latency degradation
   * - Health
     - Kubernetes/systemd
     - Process crash, hang
   * - Alerts
     - AlertManager
     - Critical: high restart rate, mass termination

Pre-Launch Checklist
--------------------

- [ ] Production build with optimizations (``-O3 -flto``).
- [ ] TOML pre-compiled to binary format.
- [ ] Mailbox capacities set and overflow policies configured.
- [ ] DLQ monitoring and alerting configured.
- [ ] Health endpoints (``/healthz``, ``/readyz``) tested.
- [ ] Metrics endpoint scraped by Prometheus.
- [ ] Log aggregation pipeline tested (Loki or ELK).
- [ ] systemd unit file deployed and enabled (if daemon mode).
- [ ] Memory limits set per region.
- [ ] Sanitizer builds pass on CI.
- [ ] Fault injection chaos tests pass on CI.
- [ ] Runbook documented for each alert.
- [ ] Graceful shutdown tested (``/system drain`` → ``/system stop``).
- [ ] Backup/restore plan for DLQ data if needed.
