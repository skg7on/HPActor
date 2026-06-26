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

## Examples

```bash
./build/apps/bench_caf/18_bench_caf --scenario actor-creation --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario mailbox-n1 --preset smoke --format csv
./build/apps/bench_caf/18_bench_caf --scenario mixed-case --preset smoke --trials 3
```

## Presets

`smoke` is suitable for quick regression checks. `nightly`, `paper-scale`, and
`stress` increase workload sizes and should be run in scheduled or manual
performance environments.
