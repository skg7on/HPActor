# Architecture Design: M:N Coroutine-Based Actor System
## Executive Summary
This document outlines the architecture for a high-concurrency Actor model implemented in C++20. It maps M user-space actors (implemented as C++20 stackless coroutines) onto N kernel threads (Workers). The system utilizes a Two-Level Scheduling architecture to decouple message delivery from CPU execution, ensuring high utilization and cache locality.

## Core Concepts
### Actor (The M): 
A lightweight, stateful, independent unit of computation. In this system, an Actor is intrinsically tied to a C++20 coroutine handle. It processes messages sequentially, ensuring thread safety for its internal state without requiring user-level mutexes.

### Worker (The N): 
A standard OS kernel thread (std::thread). Workers form a thread pool dedicated to executing Actors that are ready to run.

### Two-Level Scheduling:

Level 1 (Message to Actor): Messages are asynchronously pushed to an Actor's private Mailbox.

Level 2 (Actor to Worker): When an Actor receives a message and is not currently executing, it is promoted to a "Ready" state and pushed to a global (or work-stealing) Dispatcher Queue. Workers pull from this queue to resume the Actor's coroutine.

### Coroutine Suspension: 
If an Actor exhausts its mailbox, it co_awaits on a suspension point, returning control of the Worker thread back to the scheduler so another Actor can execute.

## Core Data Structures
### Message & Mailbox
Message: A generic envelope containing the sender's ID, the receiver's ID, and a type-erased payload (e.g., std::any or std::variant).

Mailbox: An intrusively linked, lock-free MPSC (Multi-Producer, Single-Consumer) queue attached to every Actor.

Why MPSC? Multiple actors can send messages to this actor concurrently (Multi-Producer), but only the Actor itself will ever process its own messages (Single-Consumer).

### Actor State Machine
An Actor maintains an atomic state variable to ensure it is never executed by two Workers simultaneously:

Idle: The mailbox is empty. The coroutine is suspended.

Ready: The mailbox has messages, and the Actor is waiting in the Dispatcher Queue.

Running: A Worker currently owns the Actor's coroutine handle and is executing it.

### Dispatcher (RunQueue)
RunQueue: An MPMC (Multi-Producer, Multi-Consumer) lock-free queue.

Producers: Any thread (Worker or external IO thread) that sends a message to an Idle Actor.

Consumers: The N Worker threads looking for work.

### Coroutine Primitives
ActorTask / ActorPromise: The custom C++20 return object and promise_type. The promise type controls the lifecycle of the coroutine and intercepts unhandled exceptions.

MailboxAwaiter: A custom awaitable object. When an Actor calls co_await get_next_message(), this awaiter checks the mailbox. If empty, it suspends the coroutine and sets the Actor state to Idle.

## Algorithmic Workflows
### Level 1: Message Dispatching (Sending a Message)
When Actor A sends a message to Actor B:

Actor A allocates the Message and pushes it into Actor B's Mailbox (MPSC queue).

Actor A performs an atomic Compare-And-Swap (CAS) on Actor B's state.

If B's state was Idle, the CAS succeeds in changing it to Ready. Actor A then pushes a pointer to Actor B into the global RunQueue.

If B's state was already Ready or Running, Actor A does nothing further. Actor B is already scheduled or executing and will eventually see the new message in its mailbox.

### Level 2: Worker Execution Loop (Scheduling)
Each of the N Worker threads runs an infinite loop:

Pop an Actor pointer from the RunQueue (MPMC queue).

If the queue is empty, the Worker thread yields or sleeps (e.g., using a condition variable or exponential backoff).

If an Actor is acquired, atomically set the Actor's state to Running.

Invoke the Actor's C++20 std::coroutine_handle<>::resume().

Execution: The coroutine wakes up, pops the message from its mailbox, processes it, and loops to get the next message.

Suspension: Once the mailbox is empty, the MailboxAwaiter triggers. The Actor atomically sets its state back to Idle and the coroutine suspends.

Control returns to the Worker thread, which jumps back to Step 1.

## Implementation Considerations for the Agent
Memory Management: The MPSC and MPMC queues should ideally use hazard pointers or epoch-based reclamation to prevent ABA problems without heavy locking.

Work Stealing (Future Expansion): To reduce contention on the single global RunQueue, the agent should eventually design localized queues per Worker, implementing a work-stealing algorithm (similar to Go's scheduler or Tokio in Rust).

C++20 Specifics: The agent must carefully implement the await_suspend method in MailboxAwaiter. This is the exact point where thread-safety is critical—the transition from Running to Idle must be fully resolved before returning control to the Worker.