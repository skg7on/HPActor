# HPActor Daemon Service Architecture Design

## 1. Executive Summary

HPActor currently runs as a foreground process attached to a terminal. For
production deployment on Linux, it must operate as a managed background service
that starts at boot, survives terminal logout, integrates with the init system
(systemd), and supports remote inspection via a standalone CLI client tool.

This design defines the process model, daemonization strategy, systemd
integration, signal handling, CLI client/server decoupling, health endpoints,
security boundary, and configuration extensions needed to run HPActor as a
production Linux service.

**Key decision:** Systemd-native operation is the primary recommendation.
Traditional `fork()`-based daemonization is provided as a fallback for
non-systemd environments. The application code stays clean in both modes — the
ProcessManager isolates all OS-level lifecycle concerns from the actor runtime.

## 2. Goals

1. Run HPActor as a background service managed by systemd (Type=notify).
2. Support traditional programmatic daemonization (`fork`, `setsid`, `pidfile`)
   for non-systemd Linux environments.
3. Decouple the CLI into a standalone `hpactor-cli` client tool that connects to
   the daemon via UNIX domain socket (UDS) or TCP socket.
4. Handle SIGTERM/SIGINT for graceful shutdown and SIGHUP for config reload.
5. Integrate with systemd watchdog via `sd_notify(WATCHDOG=1)`.
6. Expose health/readiness/liveness endpoints consumable by load balancers and
   orchestration systems.
7. Preserve source-compatible defaults: foreground mode is unchanged; daemon,
   systemd, and socket-listening modes are opt-in via config or CLI flags.

## 3. Non-Goals

- Container-native orchestration (Kubernetes sidecar, operator pattern) — the
  daemon mode is complementary but this design focuses on bare-metal/VM systemd.
- Socket activation (`sd_listen_fds`) in the initial implementation — the daemon
  binds its own sockets. Socket activation can be added later.
- Cross-platform daemon support — macOS `launchd` integration is deferred. The
  ProcessManager design allows it but this spec targets Linux.
- Hot code reload inside a running actor — outside the scope of daemonization.

## 4. Process Model

### 4.1 Operating Modes

The same `hpactor` binary supports three modes, selected by command-line flag
and/or TOML config:

| Mode | Flag | Terminal | Init System | Use Case |
|------|------|----------|-------------|----------|
| Foreground | `--foreground` (default) | Attached (stdin/stdout) | None | Development, debugging |
| Systemd | `--systemd` | Detached | systemd Type=notify | Production (modern Linux) |
| Daemon | `--daemon` | Detached | Generic init / manual | Production (legacy Linux) |

Mode selection precedence:
1. CLI flag (`--foreground`, `--systemd`, `--daemon`)
2. TOML config `[system.process].mode` (`"foreground"`, `"systemd"`, `"daemon"`)
3. Default: `foreground`

### 4.2 Foreground Mode (Current Behavior, Unchanged)

```
Terminal
    │
    ▼
hpactor (PID 1234)
    │
    ├── main() → ActorSystem::run()
    │       ├── CliActor (stdin/stdout, DaemonActor thread)
    │       ├── Scheduler workers (N threads)
    │       ├── EventLoop thread
    │       └── ... system actors
    │
    ▼ (Ctrl+C or /quit)
Process exits
```

Stdin is attached to the terminal. The CliActor reads from stdin via linenoise.
Logs go to stderr. SIGINT triggers graceful shutdown.

### 4.3 Systemd Mode (Recommended Production)

```
systemd
    │ fork
    ▼
hpactor (PID 1234, no terminal)
    │
    ├── ProcessManager (initialized before ActorSystem)
    │       ├── sd_notify(READY=1) after topology loaded
    │       ├── sd_notify(WATCHDOG=1) every WATCHDOG_USEC/2
    │       ├── signalfd on SIGTERM → ActorSystem::shutdown()
    │       └── sd_notify(STOPPING=1) during shutdown
    │
    ├── ActorSystem
    │       ├── CliServerActor (UDS + optional TCP listener)
    │       ├── WatchdogActor (EventBasedActor, periodic health check)
    │       ├── Scheduler workers (N threads)
    │       ├── EventLoop thread
    │       └── ... system actors
    │
    ▼ (SIGTERM or systemctl stop)
Graceful shutdown → sd_notify(STATUS=Shutting down...) → exit(0)
```

Key properties:
- No `fork()` in application code — systemd forks the process.
- stdout/stderr go to journald automatically.
- `sd_notify` is a fire-and-forget AF_UNIX datagram to `$NOTIFY_SOCKET`.
- WatchdogSec configured in service file; app sends WATCHDOG=1 at half the
  interval.

### 4.4 Traditional Daemon Mode (Legacy Fallback)

```
Terminal
    │
    ▼ (fork + exit parent)
hpactor (PID 1235, daemonized, no terminal)
    │
    ├── setsid() → new session, no controlling terminal
    ├── fork() again → not a session leader, can never acquire TTY
    ├── chdir("/")
    ├── umask(0)
    ├── close stdin/stdout/stderr → redirect to /dev/null or log files
    ├── write PID file
    │
    ├── ProcessManager
    │       ├── sigaction on SIGTERM → ActorSystem::shutdown()
    │       ├── sigaction on SIGHUP → config reload
    │       └── sigaction on SIGINT → graceful shutdown
    │
    ├── ActorSystem
    │       ├── CliServerActor (UDS + optional TCP listener)
    │       ├── Scheduler workers
    │       └── ... system actors
    │
    ▼ (SIGTERM or kill)
Graceful shutdown → remove PID file → exit(0)
```

Critical ordering rule from the issue:
> If an Actor engine starts its worker thread pool before the second `fork()`,
> those worker threads will be orphaned or broken. Always initialize your thread
> pool after daemonizing.

Therefore: **daemonize → then construct ActorSystem**. All threads (scheduler
workers, event loop, daemon actors) are spawned after the double-fork completes.
This is enforced by ProcessManager running before ActorSystem construction.

## 5. ProcessManager Subsystem

### 5.1 Ownership and Lifecycle

```
main()
  │
  ├── 1. Parse config / CLI flags
  ├── 2. ProcessManager::init(mode, config)   ← daemonize here (if applicable)
  ├── 3. ActorSystem construction             ← all threads start here
  ├── 4. ProcessManager::notify_ready()       ← signal readiness
  ├── 5. ActorSystem::run()                   ← main loop
  ├── 6. ProcessManager::notify_stopping()    ← shutdown begins
  ├── 7. ActorSystem::shutdown()
  └── 8. ProcessManager::notify_stopped()     ← cleanup (pidfile, etc.)
```

ProcessManager is a process-wide singleton, not an actor. It owns:
- Daemonization state (pidfile path, daemon mode)
- Signal handling infrastructure (signalfd or sigaction)
- systemd notification socket state

### 5.2 Interface

```cpp
namespace hpactor::process {

enum class ProcessMode : uint8_t {
    Foreground,  ///< Attached to terminal (default).
    Systemd,     ///< systemd Type=notify, no fork.
    Daemon,      ///< Traditional double-fork daemon.
};

struct ProcessConfig {
    ProcessMode mode = ProcessMode::Foreground;
    std::string pidfile_path;            ///< e.g., "/var/run/hpactor/hpactor.pid"
    bool redirect_stdio = false;         ///< Redirect stdin/out/err to /dev/null
    std::string log_file;                ///< Optional log file path for daemon mode
    std::string working_directory = "/"; ///< chdir target for daemon mode
    // systemd-specific
    std::chrono::milliseconds watchdog_interval{0};  ///< 0 = disabled
    std::string notify_socket;           ///< Override $NOTIFY_SOCKET (testing)
};

class ProcessManager {
public:
    /// Initialize process state. If mode is Daemon, performs double-fork.
    /// Must be called before ActorSystem construction.
    /// \returns error if daemonization fails.
    static result<void> init(const ProcessConfig& config);

    /// Send READY=1 to systemd, or mark internal state ready.
    static void notify_ready();

    /// Send STATUS=... to systemd.
    static void notify_status(const std::string& status);

    /// Send WATCHDOG=1 to systemd.
    static void notify_watchdog();

    /// Send STOPPING=1 to systemd.
    static void notify_stopping();

    /// Cleanup: remove pidfile, final systemd notification.
    static void notify_stopped();

    /// Current process mode.
    static ProcessMode mode();

    /// Install signal handlers and begin monitoring.
    /// \param on_terminate Callback for SIGTERM/SIGINT.
    /// \param on_reload Callback for SIGHUP.
    static result<void> install_signal_handlers(
        std::function<void()> on_terminate,
        std::function<void()> on_reload);

    /// Block until a signal arrives. Returns the signal number.
    /// For signalfd-based implementation, this integrates with event loop.
    static int wait_for_signal();

private:
    static void daemonize();  ///< Double-fork + setsid + pidfile.
    static void write_pidfile();
    static void remove_pidfile();
};

} // namespace hpactor::process
```

### 5.3 Signal Handling Design

**Linux (signalfd):**
```
signalfd_create({SIGTERM, SIGINT, SIGHUP, SIGUSR1, SIGUSR2})
    │
    ▼
EventLoop polls signalfd alongside network fds
    │
    ▼ on readable
read(signalfd) → struct signalfd_siginfo
    │
    ├── SIGTERM/SIGINT → on_terminate() → ActorSystem::shutdown()
    ├── SIGHUP         → on_reload()    → config reload
    └── SIGUSR1        → reopen logs
```

All signals are blocked in all threads. Only the event loop thread reads
signalfd. This gives deterministic, thread-safe signal handling without the
constraints of signal handler functions (no malloc, no locks, async-signal-safe
only).

**macOS / BSD fallback (kqueue EVFILT_SIGNAL):**
Equivalent to signalfd — kqueue can monitor signals directly.

**Generic fallback (self-pipe trick):**
```
sigaction handler:
    write(signal_pipe[1], &signo, sizeof(signo))  // async-signal-safe

EventLoop:
    read(signal_pipe[0]) → signo → dispatch to callback
```

### 5.4 Daemonization Sequence

```cpp
void ProcessManager::daemonize() {
    // Step 1: First fork — detach from controlling terminal
    pid_t pid = fork();
    if (pid < 0) exit(1);       // fork failed
    if (pid > 0) _exit(0);      // parent exits

    // Step 2: New session, no controlling terminal
    if (setsid() < 0) exit(1);

    // Step 3: Second fork — not a session leader, can never acquire TTY
    pid = fork();
    if (pid < 0) exit(1);
    if (pid > 0) _exit(0);

    // Step 4: Process environment
    chdir(working_directory_.c_str());
    umask(0);

    // Step 5: Redirect standard fds
    if (redirect_stdio_) {
        int devnull = open("/dev/null", O_RDWR);
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        if (devnull > STDERR_FILENO) close(devnull);
    }

    // Step 6: Write PID file
    write_pidfile();

    // NOW safe to construct ActorSystem and spawn threads.
}
```

### 5.5 PID File Management

```cpp
void ProcessManager::write_pidfile() {
    // Create parent directory if needed
    fs::path pid_path(pidfile_path_);
    fs::create_directories(pid_path.parent_path());

    // Write atomically: write to temp, then rename
    std::string tmp_path = pidfile_path_ + ".tmp";
    std::ofstream ofs(tmp_path);
    ofs << getpid() << "\n" << std::flush;
    ofs.close();
    if (ofs.fail()) {
        // Log error, continue — pidfile is not critical
        return;
    }
    rename(tmp_path.c_str(), pidfile_path_.c_str());
}

void ProcessManager::remove_pidfile() {
    unlink(pidfile_path_.c_str());
}
```

### 5.6 systemd Notify Protocol

`sd_notify` communicates over an AF_UNIX datagram socket at `$NOTIFY_SOCKET`.
If libsystemd is available, we use `sd_notify(3)`. If not, a minimal inline
implementation (~50 lines) sends the datagram directly:

```cpp
void ProcessManager::notify_ready() {
    if (mode_ != ProcessMode::Systemd) return;
    const char* socket_path = getenv("NOTIFY_SOCKET");
    if (!socket_path) return;

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strncpy(addr.sun_path, socket_path, sizeof(addr.sun_path) - 1);

    const char* msg = "READY=1";
    sendto(fd, msg, strlen(msg), MSG_NOSIGNAL,
           (struct sockaddr*)&addr, sizeof(addr));
    close(fd);
}
```

Messages sent to systemd:
| State | Message | When |
|-------|---------|------|
| Ready | `READY=1` | Topology loaded, health::ready() == true |
| Watchdog | `WATCHDOG=1` | Every WATCHDOG_USEC/2, from WatchdogActor |
| Status | `STATUS=Running N actors` | Periodic status update |
| Status | `STATUS=Draining...` | During shutdown |
| Stopping | `STOPPING=1` | Shutdown initiated |
| Error | `ERRNO=5` | On fatal errors before exit |

## 6. CliServerActor — Socket-Based CLI Server

### 6.1 Design Rationale

The current CliActor reads from stdin on a dedicated daemon thread. For daemon
and systemd modes, stdin is disconnected. Instead, we introduce
`CliServerActor` — a `DaemonActor` subclass that listens on a UNIX domain
socket (and optionally a TCP socket), accepts connections, and processes CLI
commands per connection.

The CliActor and CliServerActor share a common command processing engine via a
refactored `CliSession` class that encapsulates the command tree reference,
formatter, and request-response dispatch — decoupled from the I/O transport.

### 6.2 Component Diagram

```
┌──────────────────────────────────────────────────────┐
│                   CliServerActor                      │
│                   (DaemonActor)                       │
│                                                      │
│  run_once() loop:                                    │
│    1. accept() on UDS listener fd                    │
│    2. accept() on TCP listener fd (if enabled)       │
│    3. For each new connection → create CliSession    │
│    4. Service existing sessions (non-blocking poll)  │
│                                                      │
│  Owns:                                               │
│    - UDS listener socket                             │
│    - TCP listener socket (optional)                  │
│    - Active CliSession set                           │
│    - Shared CommandTree (from CommandRegistry)       │
└────────────────────┬─────────────────────────────────┘
                     │ creates
                     ▼
┌──────────────────────────────────────────────────────┐
│                    CliSession                         │
│                                                      │
│  Per connection state:                               │
│    - Socket fd (UDS or TCP)                          │
│    - OutputFormatter (format per client preference)  │
│    - Pager state                                     │
│    - Authentication state (none/authenticated)       │
│                                                      │
│  process_line(line) → execute_tokens() → write back  │
│                                                      │
│  Shares:                                             │
│    - CommandTree (read-only, from CliServerActor)    │
│    - ActorSystem* (for inspect/kill/list requests)  │
└──────────────────────────────────────────────────────┘
```

### 6.3 CliSession — Shared Command Processor

Extracted from the current CliActor to be transport-agnostic:

```cpp
namespace hpactor::cli {

class CliSession {
public:
    /// \param system The actor system, for sending inspect/kill/list requests.
    /// \param command_tree The trie-based command registry (shared, read-only).
    /// \param formatter Output formatter for this session.
    /// \param output_fn Callback to write formatted output back to the client.
    CliSession(ActorSystem& system,
               const CommandNode* command_tree,
               std::unique_ptr<OutputFormatter> formatter,
               std::function<void(const std::string&)> output_fn);

    /// Process a single command line. Tokenizes, walks the command tree,
    /// dispatches, and writes formatted output via output_fn.
    /// \returns true if the session should continue, false if /quit.
    bool process_line(const std::string& line);

    /// Access the pager for interactive state.
    Pager* pager() { return pager_.get(); }

private:
    void execute_tokens(const std::vector<Token>& tokens);
    // ... same send_and_wait_* helpers as current CliActor ...

    ActorSystem& system_;
    const CommandNode* command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    std::function<void(const std::string&)> output_fn_;
};

} // namespace hpactor::cli
```

### 6.4 CliServerActor Interface

```cpp
namespace hpactor::cli {

struct CliServerConfig {
    std::string uds_listen_path;      ///< UDS path, e.g., /tmp/hpactor/hpactor.sock
    mode_t uds_socket_mode = 0660;    ///< UDS permissions
    std::string uds_socket_owner;     ///< Optional owner (empty = current user)
    std::string uds_socket_group;     ///< Optional group

    uint16_t tcp_listen_port = 0;     ///< TCP port, 0 = disabled
    std::string tcp_bind_address = "127.0.0.1";  ///< TCP bind (localhost default)

    uint32_t max_sessions = 16;       ///< Max concurrent CLI sessions
    std::chrono::milliseconds session_timeout{300000};  ///< Idle session timeout (5 min)
};

class CliServerActor : public DaemonActor {
public:
    static constexpr const char* kActorTypeName = "CliServerActor";

    CliServerActor(ActorContext* ctx, ActorSystem& system,
                   const CliServerConfig& config);

    // DaemonActor interface
    bool run_once() override;
    void on_daemon_start() override;
    void on_daemon_stop() override;

    bool is_system_actor() const override { return true; }

private:
    result<void> bind_listeners();
    void accept_connections();
    void service_sessions();
    void remove_dead_sessions();

    CliServerConfig config_;
    const CommandNode* command_tree_;

    // Listener fds
    int uds_listen_fd_ = -1;
    int tcp_listen_fd_ = -1;

    // Active sessions
    struct SessionState {
        int fd;
        std::unique_ptr<CliSession> session;
        std::chrono::steady_clock::time_point last_activity;
        std::string read_buffer;     // Partial line accumulation
    };
    std::vector<SessionState> sessions_;

    // epoll/kqueue fd for non-blocking I/O multiplexing
    int poll_fd_ = -1;
};

} // namespace hpactor::cli
```

### 6.5 CliServerActor run_once() Loop

```cpp
bool CliServerActor::run_once() {
    // 1. Accept new connections (non-blocking)
    accept_connections();

    // 2. Poll existing connections for data (with ~10ms timeout)
    poll_sessions();

    // 3. Service sessions with complete lines
    service_sessions();

    // 4. Timeout idle sessions
    remove_dead_sessions();

    return running_;
}
```

Each iteration does a small amount of work and returns. The daemon thread's
outer loop calls `run_once()` repeatedly. Non-blocking I/O ensures one slow
client doesn't block others.

### 6.6 Connection Protocol

**Text mode (default):**
```
Client connects via UDS/TCP
    │
    ▼
Server: "HPActor CLI — Type /help for commands, /quit to exit.\n"
    │
Client: "/actor 5 show\n"
Server: "┌─────── Actor 0x0005 ──────┐\n...\n"  (formatted output, then)
Server: "\0"  (end-of-response sentinel)
    │
Client: "/quit\n"
Server: "Goodbye.\n\0"
    │
Connection closed
```

The end-of-response sentinel (NUL byte) allows the client to reliably parse
multi-line responses without depending on prompt characters or timing.

**JSON mode** (activated by `--format json` on first command or `-j` on client):
```
Client: "/actor 5 show --format json\n"
Server: {"result": {...}, "error": null}\n\0
```

In JSON mode, every response is a single JSON object line, making it trivially
scriptable: `echo '/actor list --format json' | hpactor-cli -j | jq`.

## 7. Standalone CLI Client (`hpactor-cli`)

### 7.1 Binary Separation

```
┌──────────────────────────┐    UDS/TCP     ┌──────────────────────────┐
│     hpactor-cli          │    connect      │     hpactor (daemon)     │
│                          │ ◄──────────────►│                          │
│  - LineEditor (linenoise)│                 │  - CliServerActor        │
│  - Terminal UI           │                 │  - ActorSystem (all      │
│  - History persistence   │                 │    actors, scheduler)    │
│  - No actor runtime      │                 │                          │
└──────────────────────────┘                 └──────────────────────────┘
```

The `hpactor-cli` binary:
- Links against hpactor_lib for CLI types (Token, Lexer, CommandContext) and
  shared protobuf messages, but does NOT create an ActorSystem.
- Contains its own LineEditor for raw terminal input with completion, hints,
  and highlighting — identical UX to the current in-process CLI.
- Connects to the daemon's CliServerActor.
- Is a thin client — validation, execution, and authorization happen on the
  server side.

### 7.2 Client CLI

```bash
# Connect via default UDS path
hpactor-cli

# Connect via explicit UDS path
hpactor-cli --socket /tmp/hpactor/system.sock

# Connect via TCP
hpactor-cli --host 127.0.0.1 --port 9876

# Non-interactive: execute a single command and exit
hpactor-cli exec "/actor list"
hpactor-cli exec --format json "/system stats"

# Pipe-friendly: read commands from stdin
echo "/system stats" | hpactor-cli exec

# With TLS (future)
hpactor-cli --host hpactor.example.com --port 9876 --tls
```

### 7.3 Client Implementation Sketch

```cpp
// tools/hpactor-cli/main.cpp
int main(int argc, char** argv) {
    CliClientConfig config = parse_args(argc, argv);

    // Connect to daemon
    int fd = connect_to_daemon(config);
    if (fd < 0) {
        fprintf(stderr, "Failed to connect to HPActor daemon: %s\n",
                strerror(errno));
        return 1;
    }

    if (config.exec_mode) {
        // Non-interactive: send command, read response, print, exit
        send_command(fd, config.command);
        std::string response = read_response(fd);
        printf("%s\n", response.c_str());
    } else {
        // Interactive mode: LineEditor + network I/O
        CliClientShell shell(fd);
        shell.run();  // blocks until /quit
    }

    close(fd);
    return 0;
}
```

## 8. WatchdogActor

### 8.1 Purpose

When systemd's `WatchdogSec=` is configured, the process must send
`WATCHDOG=1` within that interval or systemd kills it. A dedicated actor
checks system health and pings the watchdog on a periodic timer.

### 8.2 Design

```cpp
namespace hpactor::process {

class WatchdogActor : public EventBasedActor {
public:
    static constexpr const char* kActorTypeName = "WatchdogActor";

    WatchdogActor(ActorContext* ctx, ActorSystem& system,
                  std::chrono::milliseconds interval);

    Behavior make_behavior() override;

    bool is_system_actor() const override { return true; }

private:
    /// Periodic health check: is the scheduler making progress?
    /// Are critical system actors alive?
    /// If healthy → sd_notify(WATCHDOG=1)
    void on_check();

    /// Aggregate health signals
    bool is_system_healthy() const;

    std::chrono::milliseconds interval_;
    AlarmHandle timer_handle_;
};

} // namespace hpactor::process
```

### 8.3 Health Check Logic

```cpp
bool WatchdogActor::is_system_healthy() const {
    // 1. Scheduler: workers still processing messages
    auto scheduler_stats = system_.scheduler()->stats();
    if (scheduler_stats.messages_processed_since_last_check == 0 &&
        scheduler_stats.idle_duration > std::chrono::seconds(60)) {
        // Workers idle for 60s+ with no activity — might be fine
        // (quiet system) or might be stuck. Check further.
        // For now, idle is not unhealthy.
    }

    // 2. Critical system actors still alive
    if (system_.get_actor(system_.metrics_actor_id()) == nullptr) {
        return false;  // MetricsActor died
    }

    // 3. Event loop still running
    // (checked indirectly — if event loop is stuck, timer won't fire)

    return true;
}
```

Watchdog interval: typically `WatchdogSec=10` in the service file, so
`WATCHDOG_USEC=10000000`. The actor schedules `on_check()` every 5 seconds
(half the watchdog interval) via `context()->schedule_every(interval_, msg)`.

## 9. Health Endpoints

### 9.1 Integration with Existing Design

The operations-sre-design.md defines `/health/live`, `/health/ready`, and
`/health/startup` endpoints. For daemon mode, these are exposed via the
existing HTTP gateway actor (or a minimal built-in HTTP health server if ENABLE_APPS is OFF).

### 9.2 Minimal Health HTTP Server

For daemon deployments without the full HTTP gateway:

```cpp
namespace hpactor::process {

// Lightweight health-check HTTP server — single-threaded, minimal.
// Listens on a configurable port (default 8089).
// Responds to:
//   GET /health/live    → 200 OK | 503 Service Unavailable
//   GET /health/ready   → 200 OK | 503 Service Unavailable
//   GET /health/startup → 200 OK | 503 Service Unavailable
class HealthHttpServer : public DaemonActor {
public:
    HealthHttpServer(ActorContext* ctx, ActorSystem& system,
                     uint16_t port);

    bool run_once() override;  // accept + respond, non-blocking

    bool is_system_actor() const override { return true; }

private:
    void handle_request(int client_fd);

    uint16_t port_;
    int listen_fd_ = -1;
};

} // namespace hpactor::process
```

Health state is determined by the existing `ShutdownPhase` and topology-load
state:

| Endpoint | 200 Condition | 503 Condition |
|----------|---------------|---------------|
| `/health/live` | Process alive, event loop running | Process exiting / event loop stuck |
| `/health/ready` | `ShutdownPhase::Running`, topology loaded | Draining, leaving, or not yet ready |
| `/health/startup` | Topology loaded, SystemInit broadcast | Topology not yet loaded |

## 10. CliActor Refactoring for Code Reuse

The current CliActor (`src/cli/cli_actor.cpp`, ~509 lines) mixes three concerns:
1. I/O transport (stdin/stdout via LineEditor)
2. Command processing (tokenize, dispatch, execute)
3. Request-response blocking (send_and_wait_* helpers)

Extracting `CliSession` allows both CliActor (stdin) and CliServerActor
(sockets) to share the command processing logic without duplication.

### 10.1 Refactoring Plan

```
Before:
  CliActor
    ├── stdin I/O (LineEditor)
    ├── Command processing (execute_tokens, send_and_wait_*)
    └── Output (OutputFormatter, Pager)

After:
  CliSession (new, reusable)
    ├── Command processing (execute_tokens, send_and_wait_*)
    └── Output (OutputFormatter, Pager) → output_fn callback

  CliActor (refactored, foreground)
    ├── stdin I/O (LineEditor)
    └── owns CliSession

  CliServerActor (new, daemon/systemd)
    ├── UDS/TCP socket I/O
    └── owns N × CliSession
```

### 10.2 Migration Path

The existing CliActor behavior is preserved. CliSession extraction is an
internal refactor — no API breakage. Tests for CliActor (test_cli_integration,
test_command_node, test_lexer, test_formatters) continue to pass without
modification.

## 11. Configuration

### 11.1 TOML Extensions

```toml
[system.process]
# Operating mode: "foreground", "systemd", or "daemon"
mode = "systemd"

# PID file for traditional daemon mode
pidfile = "/var/run/hpactor/hpactor.pid"

# Working directory for daemon mode (chdir after fork)
working_directory = "/"

# Redirect stdin/stdout/stderr to /dev/null in daemon mode
redirect_stdio = true

# Log file for daemon mode (if empty, use syslog)
log_file = "/var/log/hpactor/hpactor.log"

[system.process.watchdog]
# Interval for WATCHDOG=1 pings. Should be half of systemd WatchdogSec.
# 0 = disabled.
interval_ms = 5000

[system.cli]
enabled = true
listen_path = "/var/run/hpactor/hpactor.sock"
uds_socket_mode = 0660
uds_socket_owner = "hpactor"
uds_socket_group = "hpactor"
tcp_port = 0                     # 0 = disabled
tcp_bind_address = "127.0.0.1"   # localhost-only by default
max_sessions = 16
session_timeout_ms = 300000      # 5 minutes
default_format = "pretty"
page_size = 50
history_max = 1000

[system.process.health]
# Health check HTTP server port (0 = disabled, use HTTP gateway instead)
http_port = 8089
http_bind_address = "127.0.0.1"
```

### 11.2 Command-Line Flags

```
--foreground            Run in foreground (default)
--systemd               Run in systemd Type=notify mode
--daemon                Run as traditional daemon (double-fork)
--pidfile <path>        PID file path (daemon mode)
--config <path>         TOML configuration file path
--log-level <level>     Override log level (trace, debug, info, warn, error)
```

### 11.3 systemd Service File Template

```ini
# /etc/systemd/system/hpactor.service
[Unit]
Description=HPActor Actor System
Documentation=https://github.com/hpactor/hpactor
After=network-online.target
Wants=network-online.target

[Service]
Type=notify
NotifyAccess=main
ExecStart=/usr/local/bin/hpactor --systemd --config /etc/hpactor/config.toml
ExecReload=/bin/kill -HUP $MAINPID

# Restart policy
Restart=on-failure
RestartSec=5s

# Watchdog
WatchdogSec=10s

# Resource limits
CPUAffinity=0-3
LimitNOFILE=65536
MemoryHigh=2G
MemoryMax=3G

# Security hardening
User=hpactor
Group=hpactor
ProtectSystem=strict
ProtectHome=yes
NoNewPrivileges=yes
PrivateTmp=yes
ReadOnlyPaths=/etc/hpactor
ReadWritePaths=/var/run/hpactor /var/log/hpactor

# Runtime directory for UDS sockets
RuntimeDirectory=hpactor
RuntimeDirectoryMode=0750

# Logging
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
```

### 11.4 Runtime Directory Structure

```
/var/run/hpactor/           (tmpfs, RuntimeDirectory)
├── hpactor.sock             (CLI UDS)
└── hpactor.pid              (PID file, daemon mode only)

/etc/hpactor/
├── config.toml              (main topology config)
├── templates.toml
└── dispatchers/
    └── pools.toml

/var/log/hpactor/
└── hpactor.log              (daemon mode log file)
```

## 12. Security

### 12.1 UDS Socket Security

- Socket created with configurable permissions (default `0660` — owner+group
  read/write only).
- Socket file is placed in a directory accessible only to the `hpactor` user
  (via `RuntimeDirectory` + systemd `User=`).
- The daemon sets socket ownership (`chown`) after bind if configured.

### 12.2 TCP Socket Security

- Binds to `127.0.0.1` by default — localhost only, no network exposure.
- If bound to a non-loopback interface, a shared-secret token authentication
  handshake is required on connect (see CLI design open question #1).
- TLS for remote TCP connections is deferred but the design accommodates it:
  `CliServerConfig::tls_context_path`.

### 12.3 Authentication Handshake

When TCP is enabled on a non-loopback interface, clients must authenticate:

```
Client connects → Server: "AUTH REQUIRED\n"
Client: "AUTH <token>\n"
Server: "AUTH OK\n" | "AUTH FAILED\n" (close connection)
```

The token is configured in TOML:
```toml
[system.cli.security]
auth_token = "${HPACTOR_CLI_TOKEN}"   # Or direct value
auth_required = true                   # Default: true for non-loopback TCP
```

The security-architecture-design.md already defines CLI commands as an
authorization resource. The token-based auth here is the first step; later
integration with the full role-based access control is straightforward.

## 13. Logging Integration

### 13.1 Mode-Dependent Log Routing

| Mode | Default Log Output | Override |
|------|-------------------|----------|
| Foreground | stderr (existing behavior) | `--log-file` |
| Systemd | stdout → journald | N/A (systemd captures) |
| Daemon | syslog | `log_file` in config |

### 13.2 Syslog Sink (New)

For traditional daemon mode without a log file:

```cpp
namespace hpactor::log {

class SyslogSink : public ILogSink {
public:
    SyslogSink(const std::string& ident = "hpactor");
    void write(const LogEntry& entry) override;
    void flush() override;
    void close() override;
};

} // namespace hpactor::log
```

systemd mode does NOT need a special sink — stdout/stderr are automatically
captured by journald. The existing `StderrSink` works correctly when stdout
goes to journal.

## 14. Implementation Phases

### Phase 1: ProcessManager Foundation (Week 1)

- `ProcessConfig` struct and `ProcessMode` enum.
- `ProcessManager` singleton with mode detection.
- Traditional daemonization (`daemonize()` with double-fork, setsid, pidfile).
- PID file write/remove with atomic rename.
- systemd `READY=1`, `STOPPING=1`, `STATUS=...` via `sd_notify` or inline
  AF_UNIX.
- Unit tests: ProcessManager mode transitions, pidfile lifecycle, sd_notify
  message format validation.

### Phase 2: Signal Handling (Week 1-2)

- signalfd-based signal monitoring on Linux.
- kqueue EVFILT_SIGNAL fallback on macOS.
- Self-pipe trick generic fallback.
- Integration test: SIGTERM → graceful shutdown sequence.

### Phase 3: CliSession Extraction (Week 2)

- Extract `CliSession` from `CliActor` into a reusable class.
- `output_fn` callback for transport-agnostic output.
- Refactor CliActor to own a CliSession with stdin/stdout callbacks.
- All existing CLI tests continue passing without modification.

### Phase 4: CliServerActor (Week 2-3)

- UDS listener with non-blocking accept.
- TCP listener (optional, localhost by default).
- Session management (create, poll, service, timeout).
- Connection protocol: text mode with NUL sentinel, JSON mode.
- Integration test: connect via UDS, send commands, verify responses.

### Phase 5: hpactor-cli Binary (Week 3)

- New `tools/hpactor-cli/` CMake target.
- UDS and TCP connection logic.
- Interactive mode with LineEditor (same UX as in-process CLI).
- Non-interactive `exec` mode for scripting.
- History persistence (~/.hpactor_cli_history).
- Smoke test: end-to-end client → daemon command execution.

### Phase 6: WatchdogActor + Health (Week 3-4)

- WatchdogActor with periodic `on_check()` timer.
- sd_notify(WATCHDOG=1) integration.
- Minimal HTTP health server (or integration with existing HTTP gateway).
- /health/live, /health/ready, /health/startup endpoints.
- Integration test: watchdog ping, health endpoint 200/503 behavior.

### Phase 7: Security + Logging (Week 4)

- UDS socket permissions and ownership.
- Token-based auth for non-loopback TCP.
- Syslog log sink for daemon mode.
- Audit log entries for CLI connections.
- Test: UDS permission validation, auth token acceptance/rejection.

### Phase 8: System Test + Packaging (Week 4-5)

- End-to-end test: systemd service file + hpactor daemon + hpactor-cli.
- Traditional daemon mode test on non-systemd Linux.
- Example systemd service file in `deploy/systemd/hpactor.service`.
- Documentation: runbook section in operations-sre-design.md.

## 15. Test Strategy

### 15.1 Unit Tests

| Component | Test File | What's Tested |
|-----------|-----------|---------------|
| ProcessManager | `test_process_manager.cpp` | Mode detection, daemonize() signaling path (not actual fork in CI), pidfile write/read/remove, sd_notify message format |
| CliSession | `test_cli_session.cpp` | Tokenize+dispatch, output_fn capture, /quit handling |
| CliServerActor | `test_cli_server_actor.cpp` | UDS bind/listen, connection accept, line framing, NUL sentinel |
| WatchdogActor | `test_watchdog_actor.cpp` | Timer scheduling, health check logic, WATCHDOG=1 call |
| HealthHttpServer | `test_health_http.cpp` | HTTP response codes for live/ready/startup states |
| SyslogSink | `test_syslog_sink.cpp` | Log entry formatting for syslog |

### 15.2 Integration Tests

| Scenario | What's Verified |
|----------|-----------------|
| Connect via UDS → execute commands | End-to-end CliServerActor + CliSession |
| Multiple concurrent sessions | Session isolation, no cross-talk |
| SIGTERM → graceful shutdown | Signal handler → shutdown coordinator sequence |
| systemd notify sequence | READY → STATUS → WATCHDOG → STOPPING |
| Client reconnection | Server handles disconnect + reconnect |
| Session timeout | Idle session cleanup |

### 15.3 System Tests

| Scenario | What's Verified |
|----------|-----------------|
| systemd service start → health check | Full daemon lifecycle under systemd |
| systemctl stop → clean exit | Graceful shutdown under systemd |
| Traditional daemon start → CLI attach → stop | Full daemon lifecycle without systemd |

## 16. Compatibility

### 16.1 Source Compatibility

- Foreground mode is unchanged. Existing users running `hpactor` in a terminal
  see no difference.
- `CliConfig::enabled = true` continues to spawn the in-process CliActor when
  mode is `foreground`.
- `CliConfig::listen_path` and `tcp_port` fields remain and are used by
  CliServerActor in daemon/systemd mode.

### 16.2 Binary Compatibility

- No breaking changes to `include/hpactor/` public headers.
- New headers under `include/hpactor/process/` for ProcessManager,
  WatchdogActor.
- `hpactor-cli` is a separate binary; `hpactor` binary size unaffected.

### 16.3 Config Compatibility

- New `[system.process]` TOML section is optional — defaults preserve current
  behavior.
- Existing `[system.cli]` fields are unchanged.

## 17. Design Rationale

### 17.1 Why systemd Type=notify Instead of Type=forking?

- `Type=forking` requires the application to daemonize itself. systemd then
  guesses when the process is ready by watching the PID file or waiting for the
  parent to exit. This is fragile.
- `Type=notify` lets the application tell systemd exactly when it's ready,
  healthy, and stopping. No guessing. No PID file races.
- The application code stays simpler — no `fork()` in the main path.
- Watchdog integration is built into the notify protocol.

### 17.2 Why Separate hpactor-cli Binary Instead of In-Process Only?

- Daemon has no terminal. A remote client is required for interactive use.
- Process separation is a security boundary: the CLI client has no access to
  actor memory, only the message-based inspect protocol.
- Multiple operators can connect simultaneously.
- The client can run on a different machine (TCP connection).
- Scripting: `hpactor-cli exec "/actor list --format json" | jq` is clean.

### 17.3 Why Non-Blocking I/O in CliServerActor?

- A single daemon thread with blocking I/O per session would stall all sessions
  when one client is slow.
- Non-blocking I/O with epoll edge-triggered polling lets one thread handle
  many sessions fairly.
- Reuses the existing event loop infrastructure (`IReactorBackend`).

### 17.4 Why signalfd Instead of Signal Handlers?

- Signal handlers have severe constraints (async-signal-safe functions only).
  They cannot safely modify non-atomic state, allocate memory, hold locks, or
  call most library functions.
- signalfd turns signals into file descriptors readable from a normal event
  loop — no constraints, no global state corruption risks.
- Same approach works on kqueue (EVFILT_SIGNAL) and via the self-pipe trick
  fallback.

### 17.5 Why CliSession Extraction Instead of Rewriting CliActor?

- The current CliActor works, is tested, and has 20+ command registrations.
- Extracting the transport-agnostic core (`CliSession`) keeps that investment
  while enabling the new socket transport.
- Risk is low: CliActor becomes a thin wrapper around CliSession.

## 18. Open Questions

1. **Socket activation**: Should CliServerActor support systemd socket
   activation (`sd_listen_fds`) so systemd manages the UDS/TCP socket file
   descriptors? This is a natural extension but adds complexity. Defer to
   Phase 2+.

2. **Multi-instance support**: If multiple hpactor processes run on one machine
   (e.g., dev + staging), how are UDS paths and ports namespaced? A
   `--instance-name` flag that suffixes paths (`/var/run/hpactor/dev.sock`)
   would resolve this.

3. **CLI binary size**: `hpactor-cli` links against protobuf and hpactor_lib
   for message types. For a truly minimal client, the protocol could be purely
   text-based (no protobuf dependency). However, sharing the tokenizer and
   formatter code saves maintenance cost. Start with the linked version;
   optimize later if binary size becomes an issue.

4. **MacOS launchd**: The ProcessManager design supports it via a
   `ProcessMode::Launchd` variant in the future. launchd sends a similar
   `LISTEN_FDS` mechanism and does not need fork daemonization. Not in scope
   for this design.

5. **Docker/container mode**: Should the container entrypoint use
   `--foreground` (PID 1, no systemd) or keep `--systemd` (with systemd as
   PID 1)? Recommendation: `--foreground` when PID 1 in a container; provide
   a separate `--container` mode if needed for health-check binding differences.

## 19. Acceptance Criteria

1. HPActor runs as a systemd Type=notify service on Linux.
2. `sd_notify(READY=1)` is sent after topology loading completes.
3. `sd_notify(WATCHDOG=1)` is sent periodically; absence triggers systemd
   restart within WatchdogSec.
4. `systemctl stop hpactor` triggers graceful shutdown via SIGTERM.
5. Traditional daemon mode works on non-systemd Linux: double-fork, setsid,
   pidfile, SIGHUP/SIGTERM handling.
6. `hpactor-cli` connects to daemon via UDS, executes commands, and displays
   formatted output identical to the in-process CLI.
7. `hpactor-cli exec "/system stats --format json"` produces machine-parseable
   output for scripting.
8. Multiple concurrent CLI sessions work without blocking each other.
9. Health endpoints return correct 200/503 based on system state.
10. UDS socket permissions are configurable and enforced.
11. Foreground mode behavior is unchanged — all existing tests pass.
12. Thread pool is initialized after daemonization in traditional daemon mode
    (no orphaned threads).
