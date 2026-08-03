# 4. Implementation

## Language & Toolchain

- Use C++20 with the repository's LLVM-style conventions.
- NEVER introduce `dynamic_cast`, `typeid`, exception-based control flow, or
  public APIs that require RTTI or exceptions.
- Exceptions are permitted ONLY in `src/config/toml_parser.cpp`,
  `src/config/toml_table_view.cpp`, and `tools/toml-compiler/compiler.cpp`
  (toml++ requires them in including TUs).

## Actor Contracts

- Use protobuf `TypedMessage` type tags for dynamic messages.
- Use typed actor signatures for static contracts.
- When changing generated protobuf contracts, TypeTag assignments, wire formats,
  binary topology, or persisted state: be explicit about compatibility and
  document backward-compatibility impact.

## Memory & Resources

- Maintain allocator ownership, memory accounting, and poisoning/canary
  assumptions when adding queues, envelopes, buffers, or actor state.
- PREFER bounded capacity with explicit failure paths over unbounded growth.
  Route pressure through typed results, backpressure signals, or DLQ policy.
- Keep blocking I/O and long-running work out of event-loop and cooperative
  scheduler paths. Use the appropriate abstraction:
  - `DaemonActor` / `BlockingActor` — blocking work on dedicated threads
  - `DenseComputingActor` — CPU-intensive work on dedicated pool
  - Async/transport abstractions — I/O that may block

## Concurrency-Sensitive Code

When changing lock-free structures, scheduler logic, mailbox internals, timer
wheels, or transport code:
- State the concurrency contract explicitly in the design.
- Add focused stress or race-oriented tests.
- Consult `docs/architecture/actor/actor-concurrency-and-lockfree-mailbox-rules.md`
  for the normative rule set on MPSC mailbox correctness, actor state ownership,
  ready-gate transitions, and concurrency test design.
