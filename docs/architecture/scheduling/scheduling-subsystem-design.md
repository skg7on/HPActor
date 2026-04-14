# Scheduling Subsystem Design

This design is split into two documents:

| Document | Contents |
| :------- | :------- |
| [Mathematical Model and Formal Definitions](scheduling-mathematical-model.md) | Notation reference, MPSC linearizability proof, HALO conditions, GEDF capacity augmentation theorem, DAG makespan bounds, PAS optimality, A2WS convergence, timing-wheel O(1) amortization, correctness properties table |
| [Detailed Architecture Design](scheduling-architecture-design.md) | Module boundaries, namespace dependency graph, class hierarchy (UML + C++), API declarations for all 9 classes, actor lifecycle state machine, message delivery flow, I/O-waiting path, interface interaction diagram |
