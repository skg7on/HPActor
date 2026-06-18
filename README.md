# HPActor

<picture>
  <source srcset="docs/project-logo/assets/hpactor-logo.png" media="(prefers-color-scheme: dark)">
  <img src="docs/project-logo/assets/hpactor-logo-preview.png" alt="HPActor logo" width="600">
</picture>

[![CI](https://github.com/skg7on/HPActor/actions/workflows/ci.yml/badge.svg)](https://github.com/skg7on/HPActor/actions/workflows/ci.yml)
[![Coverage](https://skg7on.github.io/HPActor/coverage-badge.svg)](https://skg7on.github.io/HPActor/coverage-html/)

A high-performance distributed actor framework for C++20 with million-level concurrency support. Combines work-stealing schedulers, EDF real-time scheduling, multi-priority queues, and a two-tier slab memory allocator for deterministic response times without GC pauses.

## Documentation

The **[HPActor Developer Manual](https://skg7on.github.io/HPActor/)** is the authoritative guide for building, deploying, monitoring, and operating HPActor-based systems. It covers:

| Section | Topics |
|---------|--------|
| [Getting Started](https://skg7on.github.io/HPActor/getting-started/overview.html) | Framework overview, installation, your first actor, project structure |
| [Building Applications](https://skg7on.github.io/HPActor/building-applications/actor-types.html) | Actor types, message passing, lifecycle, topology config, remote actors |
| [Monitoring](https://skg7on.github.io/HPActor/monitoring/metrics.html) | Prometheus metrics, structured logging, distributed tracing, health/readiness |
| [Operations](https://skg7on.github.io/HPActor/operations/cli.html) | Interactive CLI, CLI server, HTTP gateway, daemon mode with systemd |
| [SRE Integration](https://skg7on.github.io/HPActor/sre-integration/prometheus-grafana.html) | Prometheus + Grafana, Loki, Jaeger, AlertManager, chaos engineering |
| [Best Practices](https://skg7on.github.io/HPActor/best-practices/actor-design.html) | Actor design, error handling, performance tuning, testing, deployment |

To build the manual locally:

```bash
cd docs/manual
pip install sphinx sphinx-rtd-theme
make html
```

Built HTML lands in `docs/manual/_build/html/`.

## Recent Work (June 2026)

- **Daemon service & CLI decoupling** — ProcessManager with systemd `sd_notify` support, CliServerActor (UDS/TCP socket server), transport-agnostic CliSession, standalone `hpactor-cli` binary, HealthHttpServer, WatchdogActor, SyslogSink
- **AI accelerator subsystem** — `AcceleratorConfig` with 7 device types (CPU/GPU/NPU/FPGA/DSP/Custom), protobuf wire format, self-registering TOML parser, `ENABLE_AI_ACCELERATORS` build gate
- **Scheduler reliability hardening** — closed lost-wakeup window on x86_64 (futex-based CV wait), rate-limiter spin loop capping, adaptive worker idle model, real kernel TID display, TimingWheel edge-case fixes
- **Mailbox thread safety** — prearm race detection and fix, formal validation suite for MPSC correctness properties, ARM64 concurrency documentation
- **Reliable messaging primitives** — ACK/NACK frame types, `RetryPolicy` with exponential backoff + jitter, `OutboundDeliveryTracker`, `DeliveryReceipt`, `DeliveryPipeline`
- **Performance benchmark app** — `apps/bench_perf/` with coordinator/worker/collector/hot-actor throughput and latency benchmarks, integrated CLI
- **Documentation & process** — branch naming convention codified, CLAUDE.md/rules deduplication, new skills for deterministic tests and scheduler thread safety

## Quick Start

```bash
# Configure and build
cmake -S . -B build -GNinja
ninja -C build

# Run tests
ctest --output-on-failure --parallel 8

# Run a single test
./build/tests/unit/core/test_unit_core --gtest_filter="*ActorId*"
```

### Build Options

| Option | Default | Purpose |
|--------|---------|---------|
| `ENABLE_TSAN` / `ENABLE_ASAN` | OFF | Thread/Address sanitizers |
| `ENABLE_EXAMPLES` | ON | Build API examples |
| `ENABLE_APPS` | ON | Build complex demo apps |
| `ENABLE_PROACTOR` | OFF | Proactor backend (IoUring, GCD) |
| `ENABLE_ACTOR_METRICS` | ON | Actor-level Prometheus metrics |
| `ENABLE_ACTOR_LOGGING` | ON | Structured logging subsystem |
| `ENABLE_ACTOR_TRACING` | ON | Distributed tracing (W3C TraceContext) |
| `ENABLE_CLI` | ON | Interactive CLI (runtime opt-in) |
| `ENABLE_COVERAGE` | OFF | Code coverage instrumentation |
| `ENABLE_MEMORY_DEBUG` | OFF | Memory poisoning + canary verification |
| `ENABLE_FAULT_INJECTION` | ON | Deterministic fault injection hooks |

## Architecture

### Actor Model

HPActor is an event-based actor framework inspired by CAF. Actors are lightweight concurrency units communicating exclusively via message passing — no shared mutable state.

```
ActorRef (std::variant)
├── Actor        — shared_ptr to local actor (direct dispatch)
└── ActorProxy   — remote actor handle (transport-based send)
```

Messages are protobuf `TypedMessage` (TypeTag + payload) with sender and reply routing:

```cpp
context()->send(addr, msg);        // send to actor
context()->reply(msg);             // reply to current sender
context()->reply_with_error(code); // structured error reply
context()->schedule(delay, msg);   // self-delivery after delay
become(Behavior);                  // dynamic handler swap
co_await mailbox_awaiter;          // suspend for next message (coroutine)
```

Proto actors use declarative handler registration:

```cpp
on<T>([](T& msg) { /* fire-and-forget */ });
on_request<ReqT, ResT>([](ReqT& req) -> ResT { /* request-response */ });
```

### Actor Type Hierarchy

```
AbstractActor (interface base)
└── LocalActor (has ActorContext)
    ├── EventBasedActor (cooperative, behavior-based, coroutine-powered)
    │   ├── StatefulActor<T> (explicit state)
    │   └── DenseComputingActor (dedicated pool dispatch)
    ├── TypedEventBasedActor<Signatures...> (statically typed)
    ├── BlockingActor (thread-based, blocking receive)
    │   └── ScopedActor (main/non-actor context)
    └── DaemonActor (dedicated thread loop)
        ├── PollingActor (CPU affinity, poll budget)
        ├── ExternalMsgGatewayActor (named route table)
        │   └── HTTPGatewayActor (HTTP ingress)
        └── CliActor (interactive CLI)
```

### Subsystem Overview

| Subsystem | Key Components |
|-----------|---------------|
| **Scheduling** | `HybridScheduler` with work-stealing + A2WS, `ChaseLevDeque`, `EDFQueue`, `TimingWheel` (hierarchical O(1) timer), `CoroutineFramePool` |
| **Mailbox** | `MultiLaneQueue<T>` lock-free multi-lane queue, bounded admission, overflow handlers, backpressure signals, `DedupCache`, dead-letter queue |
| **Memory** | Two-tier slab allocator (mmap segments → thread-local caches), 8 size classes (32B–4KB), typed regions with pressure limits, hibernation/ZRAM, per-actor tracking |
| **Networking** | kqueue/epoll event loop, TLS 1.3, connection pooling, UDS, reactor/proactor backends |
| **Discovery** | Pluggable `IServiceDiscovery` — SWIM gossip, UDP registrar, hybrid, static |
| **Observability** | OpenMetrics `/metrics` (Prometheus), structured logging with pluggable sinks, W3C distributed tracing (OTLP exporter) |
| **Config** | TOML topology bootstrapping with templates, imports, AOT binary compilation; self-registering subsystem parsers |
| **Reliability** | Failure envelopes (23 canonical reasons, 12 subsystem sources), circuit breakers, quarantine, rate limiting, passivation, graceful shutdown, deterministic fault injection (80 sites, 14 domains) |
| **Supervision** | OneForOne, AllForOne, self-supervising with restart policies; remote child tracking |

### Daemon Mode

HPActor runs as a systemd service (`Type=notify`) with socket-based CLI access, health HTTP endpoint, watchdog heartbeats, and syslog logging. See the [Operations](https://skg7on.github.io/HPActor/operations/daemon-mode.html) section of the developer manual.

## Design Principles

- **C++20, no exceptions, no RTTI** — message dispatch is exception-free; `TypeTag` replaces RTTI for cross-process type identification
- **Cooperative coroutines** — thousands of actors multiplexed onto a small thread pool via C++20 stackless coroutines
- **Header-only API, compiled runtime** — actor types inline at zero cost; runtime (scheduler, event loop, networking) in shared library
- **Bounded by default** — explicit capacity limits with overflow policies; no unbounded growth
- **Deterministic testing** — no timing/thread-order assumptions; seed-replayable fault injection
- **Minimal dependencies** — system: OpenSSL, Protobuf; vendored: llhttp, toml++ v3.4.0

## Project Structure

```
include/hpactor/   — Public headers (actor, cli, config, core, fault, log, mailbox, mem, metrics, net, ref, rpc, sched, supervision, tracing, types)
src/               — Compiled runtime (hpactor_lib), including subsystem parsers in config/parsers/
tests/             — 3-tier GTest suite: unit/, integration/, system/
apps/              — Complex demo apps: order_platform, edgeops_telemetry, bench_perf
examples/          — Simple API usage examples
protos/hpactor/    — Protobuf schemas (common, frame, gossip, messages, ai_resource, etc.)
tools/             — toml-compiler (AOT), hpactor-cli (standalone client)
docs/              — Architecture specs, production reliability designs, developer manual
third_party/       — Vendored: googletest, llhttp, toml++
cmake/             — CMake modules
```

## Status

**39 GTest binaries, ~1,900 test cases, 271 test source files.** Core runtime is complete: actor lifecycle, all actor types, scheduling, networking, service discovery, remote spawn, RPC, TOML config topology, memory management, metrics, logging, tracing, CLI, failure semantics, mailbox (priority lanes, bounded admission, DLQ), supervision, fault injection, graceful shutdown, daemon service mode, AI accelerator subsystem, reliable messaging primitives, and performance benchmarks.

**Production reliability in progress:** data/control/operations plane features for 24x7 distributed operation. Delivered foundations include delivery modes, receiver deduplication, structured failure envelopes, bounded mailboxes, multi-lane priority queues, DLQ with CLI replay/export, distributed tracing, circuit breakers, quarantine, rate limiting, ask/timeout, passivation, per-message TTL enforcement, and graceful shutdown. Durable outbox/inbox, ACK/NACK retry completion, cluster control, security, and admin API remain backlog.

See the [developer manual](https://skg7on.github.io/HPActor/) for detailed architecture docs, the [production reliability roadmap](docs/architecture/production/production-reliability-plane.md), and the [refined feature-gap backlog](docs/architecture/production/feature-gap-refined-requirement-backlog.md).
