# Ops Probe, DLQ Printing, CLI & Metrics Transcript Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add an ops probe actor, dead-letter record printing, metrics snapshot, and CLI command execution to the order platform example, replacing the `--ops` mode stub and enriching `--all-in-one` post-scenario output.

**Architecture:** `OpsProbeActor` is an `EventBasedActor` that self-reschedules via `context()->schedule()` (requires #97). On each tick it reads `actor_count()`, `dead_letter_snapshot()`, `worker_count()` and prints a health line. After scenario completion, `print_dead_letter_records()` iterates and prints individual DLQ records via `pop_dead_letter()`. `print_metrics_snapshot()` spawns `MetricsActor`, sends `MetricsRequest`, and prints the OpenMetrics body. `run_cli_commands()` sends typed request messages to `CliActor` and prints the replies.

**Tech Stack:** C++20, existing `DeadLetterQueue`, `MetricsActor`, `CliActor`, `MpscRingBuffer`, `OpenMetricsFormatter`. Requires #97 for `schedule()`.

**Spec:** `docs/superpowers/specs/2026-05-17-issue-100-ops-probe-dlq-cli-metrics-design.md`

---

## File Structure

| File | Purpose |
|------|---------|
| `examples/13_order_platform.cpp` | **Modified** — add `OpsProbeActor` class, `print_dead_letter_records()`, `print_metrics_snapshot()`, `run_cli_commands()`; replace `run_long_role("OPS")` stub with `run_ops()`; enrich `run_all_in_one()` post-scenario output |
| `examples/order_platform/messages.hpp` | No changes (`OpsProbeTickTag` already defined) |

---

### Task 1: Add dead_letter_reason_name() and print_dead_letter_records()

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add reason/source name helpers**

In the anonymous namespace, before the `run_all_in_one` function:

```cpp
const char* dead_letter_reason_name(DeadLetterReason reason) {
    using R = DeadLetterReason;
    switch (reason) {
        case R::MailboxFull:           return "MailboxFull";
        case R::MailboxClosed:         return "MailboxClosed";
        case R::ActorNotFound:         return "ActorNotFound";
        case R::ActorTerminated:       return "ActorTerminated";
        case R::MissingRoute:          return "MissingRoute";
        case R::RemoteNodeUnreachable: return "RemoteNodeUnreachable";
        case R::NetworkPartition:      return "NetworkPartition";
        case R::TransportSendFailed:   return "TransportSendFailed";
        case R::DecodeFailed:          return "DecodeFailed";
        case R::OverflowPolicy:        return "OverflowPolicy";
        case R::NoDropRejected:        return "NoDropRejected";
        case R::DrainTimeout:          return "DrainTimeout";
        case R::DrainPolicyDrop:       return "DrainPolicyDrop";
        default:                       return "Unknown";
    }
}
```

- [ ] **Step 2: Add print_dead_letter_records()**

```cpp
void print_dead_letter_records(hpactor::ActorSystem& system) {
    hpactor::mailbox::DeadLetterRecord record;
    size_t count = 0;
    while (system.pop_dead_letter(record)) {
        std::cout << "  DLQ[" << count << "] "
                  << "reason=" << dead_letter_reason_name(record.reason) << " "
                  << "type_tag=0x" << std::hex << record.type_tag.value()
                  << std::dec << " "
                  << "sender_id=" << record.sender.id << " "
                  << "target_id=" << record.target.id << " "
                  << "mailbox_depth=" << record.mailbox_depth << "/"
                  << record.mailbox_capacity << " "
                  << "payload_bytes=" << record.payload_size << "\n";
        if (!record.payload_sample.empty()) {
            size_t sample_len = std::min(record.payload_sample.size(), size_t(64));
            std::string sample(record.payload_sample.begin(),
                               record.payload_sample.begin() + sample_len);
            // Print printable chars only for readability.
            std::cout << "    payload_sample: " << sample << "\n";
        }
        ++count;
    }
    if (count == 0) {
        std::cout << "  (no dead-letter records)\n";
    } else {
        std::cout << "  total: " << count << " records\n";
    }
}
```

---

### Task 2: Create OpsProbeActor

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add OpsProbeActor class**

In the anonymous namespace, after the existing actor classes:

```cpp
class OpsProbeActor : public hpactor::EventBasedActor {
  public:
    OpsProbeActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                  std::chrono::milliseconds interval = std::chrono::seconds(1))
        : hpactor::EventBasedActor(ctx, sys), interval_(interval) {
        become(make_behavior());
        // Kick off the first tick.
        context()->schedule(interval_,
            hpactor::TypedMessage(order::OpsProbeTickTag,
                                  hpactor::StreamBuffer{}));
    }

  protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() != order::OpsProbeTickTag) return;
            print_tick();
            // Self-reschedule.
            context()->schedule(interval_,
                hpactor::TypedMessage(order::OpsProbeTickTag,
                                      hpactor::StreamBuffer{}));
        }};
    }

  private:
    void print_tick() {
        auto& sys = system();
        auto* sched = sys.scheduler();

        auto dlq = sys.dead_letter_snapshot();

        std::cout << "[" << tick_ << "] "
                  << "actors=" << sys.actor_count() << " "
                  << "dlq_depth=" << dlq.depth << " "
                  << "dlq_pushed=" << dlq.total_pushed << " "
                  << "dlq_lost=" << dlq.total_lost;
        if (sched) {
            std::cout << " workers=" << sched->worker_count();
        }
        std::cout << "\n";
        ++tick_;
    }

    std::chrono::milliseconds interval_;
    uint64_t tick_ = 0;
};
```

Requires: `#include <hpactor/metrics/metrics_event.hpp>` for the ring buffer types, though `OpsProbeActor` doesn't directly need it — the `ActorSystem` accessors handle the internal state.

---

### Task 3: Implement print_metrics_snapshot()

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add the function**

```cpp
void print_metrics_snapshot(hpactor::ActorSystem& system) {
    // Spawn a MetricsActor that drains the system's ring buffer.
    // MetricsActor constructor requires a shared_ptr to the ring buffer.
    auto* ring_buffer_ptr = system.metrics_ring_buffer();
    if (!ring_buffer_ptr) {
        std::cout << "  (metrics ring buffer not available)\n";
        return;
    }

    // Create a shared_ptr from the raw pointer.
    // The ring buffer is owned by ActorSystem, so we use a no-op deleter.
    auto ring_buffer = std::shared_ptr<hpactor::metrics::MpscRingBuffer<
        hpactor::metrics::MetricEvent>>(
        ring_buffer_ptr, [](auto*) {});  // non-owning

    auto metrics_actor = system.spawn<hpactor::metrics::MetricsActor>(
        ring_buffer);

    // Send MetricsRequest and wait for the reply.
    hpactor::MetricsRequest req;
    auto future = system.send_request<hpactor::MetricsRequest,
                                       hpactor::MetricsResponse>(
        metrics_actor.id(), req);

    auto result = future.get(std::chrono::seconds(2));
    if (result) {
        std::string body(result->body.begin(), result->body.end());
        std::cout << "--- Metrics Snapshot ---\n" << body << "--- End Metrics ---\n";
    } else {
        std::cout << "  (metrics request failed: "
                  << result.error().message() << ")\n";
    }
}
```

- [ ] **Step 2: Add needed includes**

At the top of the file, add:
```cpp
#include <hpactor/metrics/metrics_actor.hpp>
#include <hpactor/metrics/metrics_event.hpp>
```

---

### Task 4: Implement run_cli_commands()

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add function for sending CLI requests programmatically**

```cpp
void run_cli_commands(hpactor::ActorSystem& system) {
    auto* cli = system.cli_actor();
    if (!cli) {
        std::cout << "  (CLI not enabled)\n";
        return;
    }

    // Send ListActorsRequest.
    hpactor::ListActorsRequest list_req;
    list_req.set_limit(50);  // max 50 actors in list
    hpactor::ListActorsReply list_reply;

    auto list_result = system.send_request_and_wait(
        cli->id(),
        hpactor::TypedMessage(TypeTag::ListActorsRequestTag,
            hpactor::ProtoTypeRegistry::serialize(list_req)),
        std::chrono::seconds(2));

    if (list_result) {
        list_reply.ParseFromArray(list_result->payload().data(),
                                   list_result->payload().size());
        std::cout << "--- Actor List ---\n";
        for (const auto& info : list_reply.actors()) {
            std::cout << "  id=" << info.actor_id()
                      << " type=" << info.actor_type()
                      << " state=" << info.state() << "\n";
        }
        std::cout << "  total: " << list_reply.actors_size() << " actors\n";
    } else {
        std::cout << "  (actor list request failed)\n";
    }

    // Send SystemStatsRequest.
    hpactor::SystemStatsRequest stats_req;
    hpactor::SystemStatsReply stats_reply;

    auto stats_result = system.send_request_and_wait(
        cli->id(),
        hpactor::TypedMessage(TypeTag::SystemStatsRequestTag,
            hpactor::ProtoTypeRegistry::serialize(stats_req)),
        std::chrono::seconds(2));

    if (stats_result) {
        stats_reply.ParseFromArray(stats_result->payload().data(),
                                    stats_result->payload().size());
        std::cout << "--- System Stats ---\n"
                  << "  total_actors=" << stats_reply.total_actors() << "\n"
                  << "  running_actors=" << stats_reply.running_actors() << "\n"
                  << "  worker_count=" << stats_reply.worker_count() << "\n"
                  << "  uptime_ms=" << stats_reply.uptime_ms() << "\n";
    } else {
        std::cout << "  (system stats request failed)\n";
    }
}
```

Note: If `send_request_and_wait()` doesn't exist on `ActorSystem`, use a `ScopedActor` (blocking actor) to send the request and block on `receive()` for the reply. Or simplify: use `deliver_local()` with polling on the mailbox in a non-actor context via `system.get_mailbox(cli->id())->try_pop()`.

**Simplified alternative:** Use the `ScopedActor` pattern since it's simpler and already exists:

```cpp
// In a ScopedActor context (blocking actor), we can send requests
// and block on the reply using receive().
```

---

### Task 5: Wire into run_all_in_one() Post-Scenario Output

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add DLQ and metrics output after scenario completion**

In `run_all_in_one()`, after the existing DLQ summary line (~line 662), add:

```cpp
// Existing:
auto dlq = system.dead_letter_snapshot();
std::cout << "DLQ depth=" << dlq.depth
          << " total_pushed=" << dlq.total_pushed
          << " total_lost=" << dlq.total_lost << "\n";

// New: detailed DLQ records.
std::cout << "--- Dead-Letter Records ---\n";
print_dead_letter_records(system);
```

And optionally after scenario completion (before `return`):

```cpp
// Metrics snapshot (optional, controlled by flag).
if (opts.metrics) {
    print_metrics_snapshot(system);
}

// CLI transcript (optional).
if (opts.enable_cli_transcript) {
    run_cli_commands(system);
}
```

- [ ] **Step 2: Add new CLI flags to Options and parse_args()**

In `struct Options`:
```cpp
bool metrics = false;
bool enable_cli_transcript = false;
int probe_interval_seconds = 1;
```

In `parse_args()`:
```cpp
} else if (arg == "--metrics") {
    opts.metrics = true;
} else if (arg == "--enable-cli-transcript") {
    opts.enable_cli_transcript = true;
} else if (arg == "--probe-interval") {
    const char* value = need_value("--probe-interval");
    if (value == nullptr) return std::nullopt;
    opts.probe_interval_seconds = std::atoi(value);
```

---

### Task 6: Replace run_ops() Stub

**File:** `examples/13_order_platform.cpp`

- [ ] **Step 1: Add run_ops() function**

```cpp
int run_ops(const Options& opts) {
    Config config = make_base_config(opts, opts.actor_port);
    config.cli.enabled = true;  // enable CLI for probe commands
    ActorSystem system(config);

    // Spawn OpsProbeActor.
    auto probe = system.spawn<OpsProbeActor>(
        std::chrono::seconds(opts.probe_interval_seconds));

    std::cout << "OPS probe running (interval="
              << opts.probe_interval_seconds << "s, "
              << "actors=" << system.actor_count() << ")\n";

    run_until_signal("OPS");

    // Final DLQ dump on exit.
    std::cout << "--- Final Dead-Letter Records ---\n";
    print_dead_letter_records(system);

    return 0;
}
```

- [ ] **Step 2: Hook into main() dispatch**

In `main()`:
```cpp
if (opts->mode == "--ops")
    return run_ops(*opts);  // was: run_long_role(*opts, "OPS");
```

---

### Task 7: Build and Manual Verification

- [ ] **Step 1: Build**

```bash
ninja -C build 13_order_platform
```

- [ ] **Step 2: DLQ records in overload scenario**

```bash
./13_order_platform --all-in-one --scenario overload
```

Expected: after SCENARIO RESULT, see detailed DLQ records with reason=MailboxFull or reason=OverflowPolicy, payload_sample shown.

- [ ] **Step 3: DLQ records in missing-route scenario**

```bash
./13_order_platform --all-in-one --scenario missing-route
```

Expected: DLQ record with reason=ActorNotFound or MissingRoute.

- [ ] **Step 4: DLQ empty in happy-path**

```bash
./13_order_platform --all-in-one --scenario happy-path
```

Expected: "(no dead-letter records)".

- [ ] **Step 5: Ops probe mode**

```bash
./13_order_platform --ops --probe-interval 1
```

Expected: health lines printed every ~1 second. Ctrl-C triggers final DLQ dump.

- [ ] **Step 6: Metrics snapshot**

```bash
./13_order_platform --all-in-one --scenario happy-path --metrics
```

Expected: OpenMetrics text printed with `# HELP`, `# TYPE`, histogram bucket lines, `# EOF`.

- [ ] **Step 7: Run full test suite**

```bash
ctest --output-on-failure --parallel 8
```

Expected: all 140 existing tests pass (no test-only changes in this issue).
