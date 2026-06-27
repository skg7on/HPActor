# bench_caf Performance Bug Fixes — Implementation Plan

**Issue:** [#378](https://github.com/skg7on/HPActor/issues/378)
**Branch:** `fix/bench-caf-perf-bugs`
**Date:** 2026-06-27

## Overview

Fix 7 bugs in the CAF benchmark app causing `mailbox-n1` to fail and poor
performance data across half the scenarios.

## Task List

### Task 1: Synchronize `max_queue_depth` with `default_capacity` overrides

**File:** `apps/bench_caf/caf_bench_scenarios.hpp`

In 6 trial functions, `mailbox.default_capacity` is overridden to a larger value
but `max_queue_depth` stays at the original 4096. Update `max_queue_depth`
alongside `default_capacity` in:

- `run_mailbox_n1_trial` (line 99)
- `run_one_to_one_trial` (line 257)
- `run_one_to_n_trial` (line 304)
- `run_n_to_n_random_trial` (line 357)
- `run_zipf_hotspot_trial` (line 599)
- `run_bursty_waves_trial` (line 659)

**Verification:** Build `18_bench_caf`, run `mailbox-n1 --preset smoke --trials 1`,
confirm `completed: true` and `total_received == 40000`.

### Task 2: Fix `bursty` expected/received mismatch

**File:** `apps/bench_caf/caf_bench_scenarios.hpp` (`run_bursty_waves_trial`)

Replace hardcoded `kMessagesPerWave` (4000) with computed `expected` (20000)
in both the while-loop condition and the `completed` check.

### Task 3: Make `pipeline` scale with preset

**File:** `apps/bench_caf/caf_bench_scenarios.hpp` (`run_pipeline_trial`)

- Add `PipelineDimensions` struct and `pipeline_dimensions_for_preset()`
- Replace hardcoded `kStages=4`/`kMessages=20` with preset-scaled values
- Smoke: 4 stages × 20 messages; Nightly: 8 × 200; PaperScale: 16 × 2000; Stress: 32 × 10000

### Task 4: Make `ring` traffic scale with preset

**File:** `apps/bench_caf/caf_bench_scenarios.hpp` (`run_ring_traffic_trial`)

- Add `RingTrafficDimensions` struct and `ring_traffic_dimensions_for_preset()`
- Replace hardcoded `kNodes=16`/`kLaps=100` with preset-scaled values
- Smoke: 16 × 100; Nightly: 32 × 500; PaperScale: 64 × 2000; Stress: 128 × 5000

### Task 5: Fix `zipf` to use actual Zipf (power-law) distribution

**File:** `apps/bench_caf/caf_bench_scenarios.hpp` (`run_zipf_hotspot_trial`)

- Add `ZipfDimensions` struct and `zipf_dimensions_for_preset()`  
- Replace `NToNRandomSender` with a new `ZipfSender` that uses power-law
  receiver selection (rank r gets weight 1/(r+1), normalized)
- Smoke: 4 senders × 4 receivers × 5000 msg; Nightly: 8 × 8 × 20000; etc.

### Task 6: Wire `cfg` into `dispatch_match` trial

**File:** `apps/bench_caf/caf_bench_micro.hpp` (`run_dispatch_match_trial`)

- Use `cfg.message_size_bytes` and `cfg.message_shape` when creating messages
- Respect `cfg.scheduler_threads` for the system config

### Task 7: Harden `RssSampler::start()`

**File:** `apps/bench_caf/caf_bench_sampler.hpp`

- Join previous worker thread (if any) before starting a new one
- Prevents data corruption if `start()` is called twice

## Build Verification

```bash
# Build only the changed target
ninja -C build apps/bench_caf/18_bench_caf

# Smoke-test mailbox-n1 (the previously-failing scenario)
./build/apps/bench_caf/18_bench_caf --scenario mailbox-n1 --preset smoke --trials 1

# Run all smoke scenarios
ctest -R bench_caf --output-on-failure
```
