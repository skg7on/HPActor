# HPActor CAF Performance Benchmark

`18_bench_caf` ports CAF benchmark scenario contracts to HPActor-native actors.
It is intended for regression testing HPActor internals, not for copying CAF
implementation code.

## Phase 1 Scenarios

| Scenario | Purpose |
|----------|---------|
| `actor-creation` | Recursive actor spawn, fan-in, teardown, and allocator pressure. |
| `mailbox-n1` | Many producers sending to one receiver mailbox. |
| `mixed-case` | Ring token passing, actor lifecycle churn, and CPU work. |

## Phase 2 Scenarios

| Scenario | Purpose |
|----------|---------|
| `traffic-one-to-one` | Baseline 1:1 send/receive overhead. |
| `traffic-one-to-n` | Fanout to N receivers, scheduler wakeup spread. |
| `traffic-n-to-n-random` | N senders to M receivers, routing and cache locality. |
| `traffic-ring` | M actors in a ring, token passing fairness and latency. |
| `traffic-pipeline` | Staged pipeline, handoff and batching effects. |
| `traffic-zipf` | Skewed Zipf receiver selection, overload concentration. |
| `traffic-bursty` | Batched bursts with idle gaps, queue growth and recovery. |

## Phase 3 Scenarios

| Scenario | Purpose |
|----------|---------|
| `message-creation` | `TypedMessage` and `StreamBuffer` construction cost. |
| `dispatch-match` | TypeTag and typed actor dispatch paths. |
| `serialization` | Protobuf encode/decode throughput. |
| `mandelbrot` | CPU-heavy Mandelbrot computation, scheduler fairness. |
| `scheduling-mix` | Concurrent spawn bursts, CPU tasks, and message rings. |
| `distributed-ping` | Cross-group ping/pong message exchange. |

## Examples

```bash
./build/apps/bench_caf/18_bench_caf --scenario actor-creation --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario mailbox-n1 --preset smoke --format csv
./build/apps/bench_caf/18_bench_caf --scenario mailbox-n1 --preset nightly --format json
./build/apps/bench_caf/18_bench_caf --scenario traffic-ring --preset smoke --format csv
./build/apps/bench_caf/18_bench_caf --scenario mandelbrot --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario distributed-ping --preset smoke --format json
```

## Presets

`smoke` is suitable for quick regression checks. `nightly`, `paper-scale`, and
`stress` increase workload sizes and should be run in scheduled or manual
performance environments.

## Message Shapes

| Shape | Meaning |
|-------|---------|
| `header-only` | Minimal `TypedMessage` payload (default). |
| `fixed-bytes` | `StreamBuffer` filled to fixed size with deterministic data. |
| `protobuf-small` | Small protobuf payload with framing markers. |
| `protobuf-nested` | Nested protobuf payload with checksum. |
| `shared-buffer` | Zero-copy candidate path. |
| `mixed-80-20` | 80% small, 20% larger payloads. |

## Traffic Distributions

| Distribution | Shape |
|--------------|-------|
| `one-to-one` | 1 sender, 1 receiver. |
| `n-to-one` | N senders, 1 receiver (hotspot). |
| `one-to-n` | 1 sender, N receivers (fanout). |
| `n-to-n-random` | N senders, M receivers (random routing). |
| `ring` | Token passing around a ring. |
| `pipeline` | Staged message forwarding. |
| `zipf-hotspot` | Skewed receiver selection. |
| `bursty-waves` | Send in bursts with idle gaps. |
