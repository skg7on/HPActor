# Scheduling Subsystem Design

This design is split into two documents:

| Document | Contents |
| :------- | :------- |
| [Mathematical Model and Formal Definitions](scheduling-mathematical-model.md) | Notation reference, MPSC linearizability proof (including ARM64 weak-ordering correction and deferred-free ring buffer bound), HALO conditions, GEDF capacity augmentation theorem, DAG makespan bounds, PAS optimality, A2WS convergence, timing-wheel O(1) amortization, correctness properties table |
| [Detailed Architecture Design](scheduling-architecture-design.md) | Module boundaries, namespace dependency graph, class hierarchy (UML + C++) including `ActorExecutionEngine`/`ActorReadyGate`, API declarations for all classes, MPSCMailbox concurrency and correctness (ARM64 visibility, TOCTOU fixup, deferred-free ring buffer), wakeup protocol and lost-wakeup prevention (edge-triggered, pre-arm race, lost-wakeup re-admission, rate-limiter spin guard), worker adaptive idle and CPU reduction (escalation model, platform-specific constants, EDF-aware CV timeout, backoff overflow prevention, timer thread deadline-based sleep), fairness and actor dispatch budgeting (RequeueReady path, `kRequeueBudget`, `kLostWakeupRequeueBudget`), actor lifecycle state machine, message delivery flow, I/O-waiting path, interface interaction diagram |
