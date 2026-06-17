.. _operations-daemon-mode:

Daemon Mode & systemd Deployment
=================================

HPActor can run as a background daemon with systemd integration,
suitable for production server deployments.

Overview
--------

.. code-block:: text

   Foreground Mode           Daemon Mode
   ┌──────────┐              ┌──────────────┐
   │ hpactor  │              │ systemd      │
   │ (stdin)  │              │   │          │
   │   │      │              │   ▼          │
   │   ▼      │              │ fork() ×2    │
   │ Actors   │              │ setsid()     │
   │ CLI (tty)│              │   │          │
   └──────────┘              │   ▼          │
                             │ hpactor(d)   │
                             │ sd_notify()  │
                             │ WatchdogActor│
                             │ CliServerActor│
                             │ SyslogSink   │
                             └──────────────┘

The :cpp:class:`ProcessManager` handles daemonization, signal handling,
and lifecycle coordination.

Configuration
-------------

.. code-block:: toml

   [system.process]
   mode = "daemon"                   # "foreground" or "daemon"
   pid_file = "/var/run/hpactor/hpactor.pid"
   work_dir = "/var/lib/hpactor"

   [system.process.watchdog]
   enabled = true
   interval_sec = 5

Enable daemon mode:

.. code-block:: bash

   cmake -DENABLE_CLI=ON ..    # CLI must be enabled

   # Run as daemon
   ./build/apps/your_app --daemon

Signal Handling
---------------

.. list-table::
   :header-rows: 1

   * - Signal
     - Behavior
   * - ``SIGTERM``
     - Graceful shutdown (drain → stop)
   * - ``SIGINT``
     - Graceful shutdown (drain → stop)
   * - ``SIGHUP``
     - Configuration reload (future)
   * - ``SIGUSR1``
     - Application-defined

Signal handling uses ``signalfd`` on Linux and a self-pipe trick on
other Unix platforms.

systemd Unit File
-----------------

.. code-block:: ini

   # /etc/systemd/system/hpactor.service
   [Unit]
   Description=HPActor Application Service
   Documentation=https://github.com/skg7on/HPActor
   After=network-online.target
   Wants=network-online.target

   [Service]
   Type=notify
   NotifyAccess=main
   ExecStart=/usr/local/bin/my_hpactor_app --daemon
   ExecReload=/bin/kill -HUP $MAINPID
   WorkingDirectory=/var/lib/hpactor
   PIDFile=/var/run/hpactor/hpactor.pid
   User=hpactor
   Group=hpactor

   # Restart policy
   Restart=on-failure
   RestartSec=10s

   # Watchdog
   WatchdogSec=10s

   # Security hardening
   NoNewPrivileges=yes
   PrivateTmp=yes
   ProtectSystem=strict
   ProtectHome=yes
   ReadWritePaths=/var/lib/hpactor /var/log/hpactor
   ReadOnlyPaths=/etc/hpactor

   # Resource limits
   LimitNOFILE=65536
   LimitNPROC=4096
   MemoryMax=2G
   CPUQuota=200%

   [Install]
   WantedBy=multi-user.target

Enabling and Starting
---------------------

.. code-block:: bash

   sudo systemctl daemon-reload
   sudo systemctl enable hpactor
   sudo systemctl start hpactor

   # Check status
   sudo systemctl status hpactor

   # View logs
   sudo journalctl -u hpactor -f

Logging in Daemon Mode
----------------------

In daemon mode, configure the syslog sink for integration with
journald:

.. code-block:: toml

   [system.log]
   level = "info"
   sinks = ["syslog", "rotating_file"]

   [system.log.syslog]
   enabled = true
   facility = "local0"

All structured logs are forwarded to syslog, which systemd-journald
picks up automatically.

Thread Safety
-------------

.. important::

   All thread creation MUST happen AFTER daemonization. The
   ``ProcessManager::init()`` call must precede ``ActorSystem``
   construction:

   .. code-block:: cpp

      int main(int argc, char* argv[]) {
          ProcessManager::init(config);   // Daemonize here
          ActorSystem system(config);     // Threads created here
          system.load_topology("topology.toml");
          system.await_shutdown();
          return 0;
      }

   This ordering ensures forked children don't inherit running threads
   or held locks.

Deployment Checklist
--------------------

1. Build with ``cmake -DENABLE_APPS=OFF -GNinja`` (production build).
2. Install the binary to ``/usr/local/bin/``.
3. Deploy the systemd unit file.
4. Place topology config at ``/etc/hpactor/topology.toml``.
5. Create data directories with correct permissions.
6. Enable and start the service.
7. Verify: ``systemctl status hpactor``, ``curl localhost:9090/healthz``.

For guidance on health checks and monitoring, see
:doc:`/monitoring/health` and :doc:`/sre-integration/prometheus-grafana`.
