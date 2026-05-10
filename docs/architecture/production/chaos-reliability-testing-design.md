# Chaos and Reliability Testing Architecture Design

## 1. Executive Summary

Unit tests prove components in isolation. A 24x7 actor system also needs tests
that prove behavior under long runtime, overload, partitions, restarts, memory
pressure, slow consumers, corrupted inputs, and rolling upgrades.

This design defines HPActor's reliability test framework: deterministic fault
injection, chaos scenarios, soak tests, fuzz tests, compatibility tests, and
production-like benchmarks.

## 2. Goals

1. Test realistic production failure modes.
2. Make fault injection deterministic enough for CI.
3. Run long soak tests outside the fast unit-test loop.
4. Fuzz network frames, TOML config, protobuf payloads, and admin endpoints.
5. Validate recovery after process and node restart.
6. Track performance and reliability regressions.

## 3. Non-Goals

- Running destructive chaos tests by default in every local build.
- Replacing unit tests.
- Depending on a specific cloud provider.

## 4. Fault Injection Points

Runtime hooks:

- mailbox admission failure
- allocator failure
- actor handler delay
- scheduler worker pause
- transport send drop
- transport receive drop
- frame corruption
- connection reset
- gossip packet loss
- clock skew
- durable store error
- config reload failure

Each hook should be controlled by test-only or configured fault policy.

## 5. Test Categories

### 5.1 Deterministic Fault Tests

Small tests that inject one fault and assert runtime response.

Examples:

- mailbox full routes to DLQ
- node down invalidates actor location cache
- duplicate reliable message is suppressed
- expired message is not delivered to actor handler

### 5.2 Chaos Scenario Tests

Multi-process or multi-node tests with randomized but recorded faults.

Examples:

- kill one node during remote RPC load
- partition cluster into two groups
- restart shard owner during handoff
- drop 20 percent of gossip packets for 60 seconds

### 5.3 Soak Tests

Long tests that check memory, latency, and stability over hours.

Examples:

- 24-hour local actor send loop
- remote send load with reconnects
- high-cardinality actor spawn/passivation
- bounded mailbox pressure with slow consumers

### 5.4 Fuzz Tests

Inputs:

- TOML topology files
- wire frames
- protobuf decode paths
- HTTP parser
- CLI lexer
- admin API requests

Expected behavior:

- no crash
- no OOM
- no undefined behavior under sanitizers
- structured error or rejection

### 5.5 Compatibility Tests

Matrix:

- current node to previous node
- feature-enabled to feature-disabled
- protocol min/max negotiation
- old config to new parser
- old binary topology to new loader

## 6. Test Harness

Components:

- `FaultController`: enables and disables fault points.
- `ClusterHarness`: starts local multi-process clusters.
- `NetworkProxy`: injects delay, drop, duplication, and partition.
- `SoakRunner`: records metrics over time and checks thresholds.
- `ReplayLog`: records random seed and injected fault timeline.

## 7. CI Strategy

Fast lane:

- unit tests
- deterministic fault tests
- parser fuzz smoke
- sanitizer build sample

Nightly lane:

- chaos scenarios
- soak tests
- larger fuzz corpora
- compatibility matrix
- performance regression suite

Release lane:

- full sanitizer matrix
- multi-hour soak
- rolling upgrade compatibility
- security scan and config validation

## 8. Acceptance Criteria

- Fault injection hooks exist for mailbox, network, scheduler, allocator, and
  durable storage.
- Chaos tests can reproduce failures from a saved seed.
- Soak tests record memory and latency trends.
- Fuzz tests reject malformed inputs without crashing.
- Compatibility tests cover at least one previous protocol/config version.
- Reliability failures produce enough logs and metrics to debug the scenario.

