# Ops Probe, DLQ Printing, CLI & Metrics Transcript — Design

**Date:** 2026-05-17
**Status:** Draft
**Issue:** [#100](https://github.com/skg7on/HPActor/issues/100)
**Dependency on:** #97 (ActorContext::schedule) — needed for timer-driven OpsProbeActor periodic tick

---

## Overview

Add an ops probe actor and transcript output to the order platform example, demonstrating the framework's metrics, CLI introspection, and dead-letter queue surfaces. This replaces the `--ops` mode stub and enriches `--all-in-one` post-scenario output with structured DLQ records and OpenMetrics text.

## Current State

- `--ops` mode: calls `run_long_role("OPS")` — spawns `ActorSystem` with network, prints endpoint, sleeps until Ctrl-C. No probe actor.
- `--all-in-one` mode: prints a one-line DLQ summary (`DLQ depth=N total_pushed=N total_lost=N`) but does not print individual dead-letter records.
- `MetricsActor` is fully implemented but never spawned or scraped in any example.
- `CliActor` has `/actor list`, `/actor <id> show`, `/system stats` commands implemented but not exercised by the order platform example.
- `DeadLetterQueue` has `try_pop(record)` built but dead-letter records are never individually inspected.
- `OpsProbeTickTag` (`0x00020011`) is defined in `messages.hpp` but unused.

## Goals

1. **OpsProbeActor** — a timer-driven `EventBasedActor` that periodically collects and prints system health:
   - Actor count (running / total)
   - DLQ depth and total pushed
   - Scheduler utilization
   - Memory active bytes (if memory tracking enabled)
2. **DLQ record printing** — after scenario completion (in `--all-in-one` mode), iterate and print individual dead-letter records with reason, sender, target, and payload sample
3. **Metrics snapshot** — in `--all-in-one` mode, spawn `MetricsActor`, send `MetricsRequest`, and print the OpenMetrics text output after scenario completion
4. **CLI transcript** — in `--ops` mode, enable CLI and print the output of `/actor list`, `/system stats`, and `/actor <id> show` commands periodically
5. **Integration** — wire into `--ops` mode and `--all-in-one` mode
6. No new external dependencies

## Non-Goals

- Persistent metrics storage (Prometheus scraping) — text output to stdout only
- Full CLI interactive mode in the example — programmatic CLI command execution only
- Grafana dashboard definitions
- Alerting thresholds or incident auto-ticketing

---

## Architecture

### Component Diagram

```
┌─────────────────────────────────────────────────────────┐
│ ActorSystem (all-in-one or ops mode)                     │
│                                                          │
│  ┌──────────────────────┐   ┌─────────────────────────┐ │
│  │ OpsProbeActor        │   │ MetricsActor            │ │
│  │ (EventBasedActor)    │   │ (EventBasedActor)       │ │
│  │                       │   │                          │ │
│  │ behavior:             │   │ on_request<Req, Resp>:  │ │
│  │  on OpsProbeTick ─────┼──▶│  drain ring buffer      │ │
│  │   → print system stats│   │  aggregate              │ │
│  │   → self-schedule()   │   │  format OpenMetrics     │ │
│  │                       │   │  reply with text body   │ │
│  └──────────────────────┘   └─────────────────────────┘ │
│           │                          ▲                   │
│           │ schedule(1s,              │ MetricsRequest   │
│           │   OpsProbeTick)           │                  │
│           ▼                          │                  │
│  ┌──────────────────────┐   ┌────────┴────────────────┐ │
│  │ TimingWheel          │   │ DeadLetterQueue         │ │
│  │ (via IScheduler)     │   │  snapshot() / pop()     │ │
│  └──────────────────────┘   └─────────────────────────┘ │
└─────────────────────────────────────────────────────────┘
```

### Data Flow

```
OpsProbeActor behavior:
  on OpsProbeTick:
    ├─ system.actor_count()          → "actors: N running, M total"
    ├─ system.dead_letter_snapshot() → "dlq: depth=N pushed=M lost=K"
    ├─ scheduler->worker_count()     → "workers: N"
    ├─ (optional) memory stats       → "memory: N bytes active"
    ├─ print transcript line with timestamp
    └─ context()->schedule(1s, OpsProbeTick)  // self-reschedule

Post-scenario (all-in-one mode):
    ├─ print_dlq_records(system):
    │   while (system.pop_dead_letter(record)):
    │       print "  reason=X sender=Y target=Z type_tag=W payload_sample=..."
    │
    ├─ print_metrics(system):
    │   auto* metrics_actor = system.get_actor(metrics_actor_id)
    │   auto response = send_and_wait(MetricsRequest, MetricsActor)
    │   print response.body  // OpenMetrics text
    │
    └─ (optional) cli_commands(system):
        cli_actor.execute("/actor list")
        cli_actor.execute("/system stats")
```

---

## Detailed Design

### 1. OpsProbeActor

A new `EventBasedActor` defined inline in the example file (no new framework header needed — it's example-specific):

```cpp
class OpsProbeActor : public EventBasedActor {
  public:
    OpsProbeActor(ActorContext* ctx, ActorSystem& sys,
                  std::chrono::milliseconds interval = std::chrono::seconds(1))
        : EventBasedActor(ctx, sys), interval_(interval) {
        become(make_behavior());
        // Kick off the first tick.
        context()->schedule(interval_,
            TypedMessage(OpsProbeTickTag, StreamBuffer{}));
    }

  protected:
    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != OpsProbeTickTag) return;
            print_tick();
            // Self-reschedule for next tick.
            context()->schedule(interval_,
                TypedMessage(OpsProbeTickTag, StreamBuffer{}));
        }};
    }

  private:
    void print_tick() {
        auto& sys = system();
        auto* sched = sys.scheduler();

        auto dlq = sys.dead_letter_snapshot();

        std::ostringstream line;
        line << "[" << tick_ << "] ";
        line << "actors=" << sys.actor_count() << " ";
        line << "dlq_depth=" << dlq.depth << " "
             << "dlq_pushed=" << dlq.total_pushed << " ";
        if (sched) {
            line << "workers=" << sched->worker_count() << " ";
        }
        std::cout << line.str() << "\n";
        ++tick_;
    }

    std::chrono::milliseconds interval_;
    uint64_t tick_ = 0;
};
```

Key design decisions:
- **Self-rescheduling pattern**: the handler for `OpsProbeTickTag` calls `context()->schedule(interval_, next_tick_msg)` to keep the probe running. This avoids needing a recurring timer API.
- **No DaemonActor**: using `EventBasedActor` + `schedule()` keeps the probe on worker threads (cooperative) rather than a dedicated thread. The probe's work is just a few atomic reads and a `cout` write — fast enough for the worker pool.
- **Requires #97**: `context()->schedule()` must be implemented for the self-rescheduling pattern to work.
- **Interval configurable**: default 1s, exposed as constructor parameter.

### 2. DLQ Record Printing

A free function that iterates dead-letter records:

```cpp
void print_dead_letter_records(hpactor::ActorSystem& system) {
    DeadLetterRecord record;
    size_t count = 0;
    while (system.pop_dead_letter(record)) {
        std::cout << "  DLQ[" << count << "] "
                  << "reason=" << dead_letter_reason_name(record.reason) << " "
                  << "source=" << dead_letter_source_name(record.source) << " "
                  << "type_tag=0x" << std::hex << record.type_tag << std::dec << " "
                  << "sender=" << record.sender.id << " "
                  << "target=" << record.target.id << " "
                  << "depth=" << record.mailbox_depth << "/"
                  << record.mailbox_capacity << " "
                  << "payload_bytes=" << record.payload_size << "\n";
        if (!record.payload_sample.empty()) {
            std::string sample(record.payload_sample.begin(),
                             std::min(record.payload_sample.size(), size_t(64)));
            std::cout << "    payload_sample: " << sample << "\n";
        }
        ++count;
    }
    if (count == 0) {
        std::cout << "  (no dead-letter records)\n";
    } else {
        std::cout << "  total dead-letter records: " << count << "\n";
    }
}

const char* dead_letter_reason_name(DeadLetterReason reason) {
    switch (reason) {
        case DeadLetterReason::MailboxFull:  return "MailboxFull";
        case DeadLetterReason::ActorNotFound: return "ActorNotFound";
        case DeadLetterReason::MissingRoute: return "MissingRoute";
        // ... rest of enum values
        default: return "Unknown";
    }
}
```

Note: `pop_dead_letter()` removes records from the queue. This is a destructive read — appropriate for a post-mortem transcript.

### 3. Metrics Scraping

In `--all-in-one` mode, after the scenario completes, spawn `MetricsActor` and send it a `MetricsRequest`:

```cpp
void print_metrics_snapshot(hpactor::ActorSystem& system) {
    // MetricsActor needs the ring buffer from the system.
    // The ring buffer was created during ActorSystem construction
    // and is accessible via system.metrics_ring_buffer().
    
    auto metrics_actor = system.spawn<hpactor::metrics::MetricsActor>(
        system.metrics_ring_buffer());
    
    // Send MetricsRequest and wait for response.
    hpactor::MetricsRequest req;
    auto future = system.send_request<hpactor::MetricsRequest,
                                      hpactor::MetricsResponse>(
        metrics_actor.id(), req);
    
    auto result = future.get(std::chrono::seconds(2));
    if (result) {
        std::string body(result->body.begin(), result->body.end());
        std::cout << "--- Metrics Snapshot ---\n" << body << "\n";
    }
}
```

**Important note**: `MetricsActor` currently takes `MpscRingBuffer<MetricEvent>*` in its constructor. Looking at the source (`metrics_actor.hpp`), the constructor is:

```cpp
MetricsActor(ActorContext* ctx, ActorSystem& sys,
             std::shared_ptr<MpscRingBuffer<MetricEvent>> ring_buffer);
```

The `ActorSystem` currently creates the ring buffer at construction but never stores a `MetricsActor` reference — it stores the `MpscRingBuffer` as `metrics_ring_buffer_`. The `spawn<>()` with constructor args must pass the ring buffer.

**Alternative approach**: If `MetricsActor` constructor doesn't support spawn-with-args through the current template machinery, provide a `spawn_metrics_actor()` helper or use the lower-level `AbstractActor` spawn path. The `ActorSystem::spawn<T>(Args...)` variadic template should handle this.

### 4. CLI Command Execution

For programmatic CLI command execution (not interactive), we can send CLI request messages directly to the `CliActor`:

```cpp
void run_cli_commands(ActorSystem& system) {
    auto* cli = system.cli_actor();
    if (!cli) {
        std::cout << "(CLI not enabled)\n";
        return;
    }

    // Send ListActorsRequest.
    hpactor::ListActorsRequest list_req;
    list_req.set_limit(20);
    
    auto list_future = system.send_request<hpactor::ListActorsRequest,
                                            hpactor::ListActorsReply>(
        cli->id(), list_req);
    auto list_result = list_future.get(std::chrono::seconds(2));
    if (list_result) {
        std::cout << "--- Actor List ---\n"
                  << list_result->DebugString() << "\n";
    }

    // Send SystemStatsRequest.
    hpactor::SystemStatsRequest stats_req;
    auto stats_future = system.send_request<hpactor::SystemStatsRequest,
                                             hpactor::SystemStatsReply>(
        cli->id(), stats_req);
    auto stats_result = stats_future.get(std::chrono::seconds(2));
    if (stats_result) {
        std::cout << "--- System Stats ---\n"
                  << "total_actors=" << stats_result->total_actors() << "\n"
                  << "running_actors=" << stats_result->running_actors() << "\n"
                  << "worker_count=" << stats_result->worker_count() << "\n";
    }
}
```

Note: `send_request<Req, Resp>()` on `ActorSystem` may not exist yet. Alternative: deliver the request message directly and poll for the response via `context()->send()` + `mailbox()->try_pop()` in a non-actor context. Or use the `ScopedActor` pattern (a `BlockingActor` that can block on `receive()`).

### 5. Integration into Example Modes

**`--all-in-one` mode** (post-scenario):
```cpp
int run_all_in_one(const Options& opts) {
    // ... existing setup and scenario execution ...

    // After scenario completes:
    auto final_status = result_future.get();
    std::cout << "SCENARIO RESULT ...\n";

    // DLQ transcript.
    std::cout << "--- Dead-Letter Queue ---\n";
    print_dead_letter_records(system);

    // Metrics snapshot.
    print_metrics_snapshot(system);

    // CLI transcript (if CLI enabled in opts).
    if (opts.enable_cli_transcript) {
        run_cli_commands(system);
    }

    return 0;
}
```

**`--ops` mode** (long-running with periodic probe):
```cpp
int run_ops(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.cli.enabled = true;  // enable CLI for probe commands
    ActorSystem system(config);

    // Spawn OpsProbeActor.
    auto probe = system.spawn<OpsProbeActor>(
        std::chrono::seconds(opts.probe_interval_seconds));

    std::cout << "OPS probe running (interval="
              << opts.probe_interval_seconds << "s). Press Ctrl-C to stop.\n";
    run_until_signal("OPS");

    // Final DLQ dump on exit.
    std::cout << "--- Final DLQ ---\n";
    print_dead_letter_records(system);

    return 0;
}
```

### 6. New Command-Line Options

| Flag | Default | Description |
|------|---------|-------------|
| `--probe-interval` | `1` | OpsProbeActor tick interval in seconds |
| `--enable-cli-transcript` | `false` | Print CLI actor list and system stats after scenario |
| `--metrics` | `false` | Print OpenMetrics text after scenario completion |

---

## Changes Summary

### Files Modified

| File | Change |
|------|--------|
| `examples/13_order_platform.cpp` | Add `OpsProbeActor` class, `print_dead_letter_records()`, `print_metrics_snapshot()`, `run_cli_commands()`; replace `run_long_role("OPS")` stub; enrich `run_all_in_one()` post-scenario output |
| `examples/order_platform/messages.hpp` | No changes needed (`OpsProbeTickTag` already defined) |

### New Files

None. All additions are inline in the example.

### Files Referenced (no changes)

| File | Role |
|------|------|
| `include/hpactor/mailbox/dead_letter_queue.hpp` | `DeadLetterRecord`, `DeadLetterReason`, `DeadLetterSource`, `DeadLetterQueueSnapshot` |
| `src/mailbox/dead_letter_queue.cpp` | `DeadLetterQueue::try_pop()` |
| `include/hpactor/metrics/metrics_actor.hpp` | `MetricsActor` |
| `include/hpactor/metrics/metrics_event.hpp` | `MetricEvent`, `MetricEventType` |
| `include/hpactor/cli/cli_actor.hpp` | `CliActor` |
| `protos/hpactor/cli_messages.proto` | `ListActorsRequest/Reply`, `SystemStatsRequest/Reply` |
| `include/hpactor/core/actor_system.hpp` | `actor_count()`, `dead_letter_snapshot()`, `pop_dead_letter()`, `cli_actor()`, `metrics_ring_buffer()` |

---

## Acceptance Criteria

1. `--all-in-one --scenario overload` prints individual DLQ records with reason, sender, target, and payload sample after "SCENARIO RESULT"
2. `--all-in-one --scenario happy-path` prints "(no dead-letter records)" after scenario completion
3. `--ops` mode prints periodic health lines like `[0] actors=5 dlq_depth=0 dlq_pushed=0 workers=4` every second until Ctrl-C
4. `--all-in-one --metrics` prints OpenMetrics text including `hpactor_mailbox_enqueue_total` and `hpactor_actor_processing_seconds` histogram buckets
5. `--all-in-one --enable-cli-transcript` prints actor list and system stats after scenario completion
6. Ctrl-C on `--ops` mode triggers final DLQ dump then clean exit
7. No new external dependencies
8. Depends on #97 (`ActorContext::schedule()`) for `OpsProbeActor` self-rescheduling pattern

## Test Plan

### Manual integration tests

1. `./13_order_platform --all-in-one --scenario overload`
   - Verify: DLQ records printed with reason=MailboxFull or reason=OverflowPolicy
   - Verify: payload_sample shown for each record

2. `./13_order_platform --all-in-one --scenario missing-route`
   - Verify: DLQ record with reason=ActorNotFound or reason=MissingRoute

3. `./13_order_platform --all-in-one --scenario happy-path --metrics`
   - Verify: OpenMetrics text printed with `# HELP`, `# TYPE`, `_bucket`, `# EOF` lines

4. `./13_order_platform --ops --probe-interval 1`
   - Verify: health lines printed every ~1 second, final DLQ dump on Ctrl-C

### Automated test

- `test_order_platform_messages.cpp`: existing round-trip tests continue to pass
