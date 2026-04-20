# **Software Architecture Design Specification: High-Concurrency Event-Driven Actor System**

## **1\. Introduction**

This document provides the formal software design specification for a high-performance distributed actor system.

The evolution of high-performance distributed computing has consistently sought to resolve the impedance mismatch between limited physical hardware resources and the demand for massively concurrent logical operations. Traditional server architectures typically follow a thread-per-connection or thread-per-task model, wherein the underlying operating system allocates a dedicated physical thread for every concurrent operation. As system demands scale into the millions of concurrent tasks, this traditional model collapses under its own weight. The operating system becomes overwhelmed by the catastrophic overhead of context switching, where the processor expends more computational cycles thrashing between thread contexts than executing actual business logic. Furthermore, the memory footprint required to maintain millions of thread stacks—often defaulting to one megabyte per thread—results in rapid and fatal memory exhaustion.

To circumvent these profound hardware and operating system limitations, modern ultra-high-throughput environments rely on the Actor Model of concurrent computation, a paradigm originally conceptualized by Hewitt, Bishop, and Steiger in 1973\. The architecture detailed in this comprehensive research document presents an advanced, event-driven actor system designed to seamlessly host millions of logical actors on a highly constrained, configurable thread pool. This system completely decouples logical execution paths from physical thread allocation, relying instead on sophisticated asynchronous message passing and robust state encapsulation.

## **2\. System Overview**

The system architecture provides a high-level orchestration of components to meet extreme concurrency goals.

The architecture achieves this massive multiplexing through a highly orchestrated triad of core components. The first component is the "Gate," an advanced event demultiplexer and ingestion engine that continuously monitors and polls both external network sockets and internal inter-actor communication channels. The Gate acts as the system's central nervous system, detecting asynchronous stimuli and transforming them into standardized events. Upon detecting an event, the Gate notifies the second core component: the Pluggable Scheduler. The Scheduler evaluates all active logical actors and applies highly specialized algorithms to dictate the precise order of execution. The scheduling engine supports a wide array of configurations, including Adaptive Asynchronous Work-Stealing (A2WS), Earliest Deadline First (EDF), Priority-based Rate Monotonic (RM) scheduling, and Round Robin (RR). The third and final component is the Configurable Thread Pool, a bounded collective of physical working threads that executes the mathematical and behavioral logic of the actors precisely as directed by the Scheduler.

This document provides an exhaustive, low-level technical analysis of these architectural paradigms, exploring the mathematical models governing scheduling behavior, the nuanced data structures required for lock-free memory management, and the second-order implications for multiprocessor efficiency and cache locality.

## **3\. Architectural Strategies**

This section outlines the generalized approaches to control, concurrency, and synchronization within the system.

### **3.1 The Logical Actor Subsystem**

The foundational building block of this architecture is the logical actor. In strict contrast to traditional multithreaded design, an actor is a purely virtual resource, mapped entirely within the user space of the application application. A logical actor is an independent, stateful entity that encapsulates its own internal data, defines a specific set of behavioral operations, and possesses a dedicated message queue known as a mailbox.

Because logical actors do not possess a dedicated operating system thread, they are phenomenally lightweight. An actor in an idle state consumes only the heap memory required for its state variables and the object overhead of its mailbox. This fundamental design choice allows the system to initialize, host, and route messages between literally millions of logical actors simultaneously, constrained solely by the total available physical Random Access Memory (RAM) of the host node.

### **3.2 State Encapsulation and the Shared-Nothing Principle**

The actor system is governed by the "shared-nothing" principle. Actors never share memory space, nor do they ever directly invoke the methods or access the internal variables of another actor. All communication, whether querying a state or issuing a command, occurs strictly through the transmission of immutable messages. This architectural mandate entirely neutralizes the most pernicious problems of parallel computing: race conditions, deadlocks, and the necessity for complex mutex locking protocols.

By enforcing strict encapsulation, the system guarantees that only one physical thread from the configurable thread pool will ever access an actor's internal state at any given discrete moment in time. When an actor is dispatched for execution, the assigned thread processes the messages within the actor's mailbox sequentially. This single-threaded execution context within the boundary of the individual actor eliminates the need for internal synchronization primitives, allowing the developer to write code as though the system were entirely synchronous, despite the underlying architecture being massively concurrent and asynchronous.

### **3.3 Lock-Free Mailbox Architectures**

While the internal state of the actor requires no locking, the actor's mailbox represents a highly volatile boundary layer. Millions of actors, network interfaces, and concurrent threads may attempt to deliver messages to a single destination actor's mailbox simultaneously. Implementing a traditional locking mechanism on the mailbox would introduce severe contention, effectively stalling the thread pool and degrading overall system throughput.

To resolve this, the mailbox is architected as a lock-free Multi-Producer, Single-Consumer (MPSC) queue. This sophisticated data structure leverages atomic hardware instructions, such as Compare-And-Swap (CAS), to allow virtually unlimited concurrent threads to enqueue messages without ever blocking the producer. Conversely, because the Scheduler guarantees that an actor is only ever assigned to a single worker thread at a time, the dequeue operation remains strictly single-consumer, further simplifying the atomic operations required to extract messages for processing.

### **3.4 The Lifecycle and State Machine of a Logical Actor**

To manage the execution of millions of actors across a highly limited pool of physical threads, the architecture enforces a rigorous, multi-staged state machine for every logical actor. This state machine dictates the actor's relationship with the Gate, the Scheduler, and the Configurable Thread Pool.

The lifecycle begins in the **IDLE** state. In this state, the actor's mailbox is entirely empty. The actor consumes absolute zero Central Processing Unit (CPU) cycles and is essentially invisible to the Scheduler and the Thread Pool. It exists purely as a passive data structure in the heap memory.

When a new message is directed to the actor, the system transitions the actor to the **SIGNALED** state. The Gate, acting as the system's demultiplexer, intercepts the incoming event—whether it originates from a network socket or another local actor—and atomically enqueues the message into the actor's MPSC mailbox. Critically, the Gate recognizes that the actor was previously dormant and immediately issues a notification to the Scheduler, indicating that the actor now possesses pending work and requires computational resources.

Upon receiving this notification, the Scheduler assesses the actor and transitions it to the **READY** state. The actor is injected into the Scheduler's internal run-queues, the exact structure of which is determined by the active algorithmic policy (e.g., a priority queue for Earliest Deadline First, or a localized double-ended queue for Work-Stealing). The actor waits in this queue until a physical thread becomes available.

When a worker thread completes its previous task, it queries the Scheduler, dequeues the highest-priority actor, and transitions it to the **RUNNING** state. The physical thread binds to the actor and begins executing its defined behavioral logic, processing the messages sequentially from the mailbox. To prevent a single highly-loaded actor from monopolizing a physical thread and starving the rest of the system, the architecture enforces a configurable throughput quota or time slice.

If the actor successfully processes all messages in its mailbox, it yields the thread and returns to the **IDLE** state. If the actor reaches its throughput quota but messages remain in the mailbox, it transitions to a **SUSPENDED** or **YIELDING** state, returning the physical thread to the pool and immediately placing itself back into the Scheduler's run-queue in the **READY** state. This cooperative multitasking model ensures absolute fairness and fluidity across millions of concurrent logical entities.

## **4\. System Architecture**

This section defines the primary structural components of the system.

### **4.1 Component 1: The Gate (Unified Event Demultiplexer)**

In complex distributed architectures, performance bottlenecks frequently materialize at the boundary between the operating system's networking stack and the user-space application. Polling-based event handling, excessive system call invocations for Input/Output (I/O) operations, and the exhaustion of file descriptors are common culprits of systemic degradation. The architecture detailed in this report mitigates these constraints through the implementation of the "Gate," an advanced unified event demultiplexer that embodies the Active Object design pattern.

The Gate serves as the singular ingestion point for all stimuli entering the actor system. It effectively decouples the detection and receipt of an event from the execution of the logic required to process that event. By acting as a communication gateway, the Gate routes messages from myriad producer processes—both local and distributed—to the consumer logical actors without imposing direct dependencies among them.

#### **4.1.1 External Network Socket Polling and High-Performance I/O**

For external interactions, the Gate interfaces directly with the host operating system's kernel to monitor potentially millions of concurrent network connections. Allocating a physical thread to block and wait on each individual network socket is architecturally unviable due to context-switching overhead. Instead, the Gate utilizes high-performance, non-blocking I/O multiplexing primitives provided by modern operating systems, such as epoll in Linux, kqueue in FreeBSD and macOS, or I/O Completion Ports in Windows environments.

The Gate operates a dedicated event loop. It registers all active network sockets with the kernel's multiplexer. When a network interface card (NIC) receives a data packet, the hardware issues an interrupt, and the kernel subsequently notifies the Gate that a specific file descriptor is ready for reading. The Gate performs a non-blocking read operation, extracts the raw byte stream, and performs immediate protocol framing and deserialization. The raw byte array is transformed into a strongly typed, immutable message object, which the Gate then deposits directly into the target actor's MPSC mailbox.

To achieve extreme, ultra-low latency performance necessary for domains like high-frequency trading or telecommunications, the Gate can be configured to bypass the standard POSIX socket API entirely. By integrating with advanced kernel-bypass frameworks such as the Data Plane Development Kit (DPDK), the Gate maps the physical network interface cards directly into the application's user space memory. This entirely eliminates the overhead of kernel-to-user space memory copying and kernel context switches, allowing the Gate to poll raw memory rings for incoming packets at wire speed.

#### **4.1.2 Internal Event Routing and State Notification**

A defining characteristic of this architecture is that the Gate is not restricted to external network traffic; it is equally responsible for monitoring and routing internal events. When a logical actor operating on the same host needs to communicate with another local actor, it does not insert the message directly into the recipient's mailbox. Instead, it dispatches the message to the Gate.

Routing internal events through the Gate provides a centralized mechanism for system auditing, distributed tracing, and, most importantly, state transition management. The Gate acts as the ultimate sentinel. When it deposits an internal message into a destination actor's mailbox, it evaluates the actor's atomic state flag. If the Gate determines that the actor was previously in the IDLE state, it generates a high-priority notification to the Scheduler, indicating that an event has occurred and the actor must be dispatched to the thread pool. This design pattern strictly isolates the actors from the underlying scheduling infrastructure, allowing them to remain purely reactive entities.

#### **4.1.3 Event Batching and Yielding Mechanisms**

Because the Gate handles 100% of the system's incoming traffic and internal signaling, it is highly susceptible to becoming a single point of failure or a severe CPU bottleneck. To maintain fluidity, the Gate incorporates advanced throughput optimizations. Primary among these is event batching. Rather than notifying the Scheduler individually for every single message received, the Gate aggregates multiple small events into substantial batches before interacting with the Scheduler's input queues. This significantly reduces the frequency of lock acquisitions and cross-thread signaling, dramatically enhancing the system's ability to scale across multiple processor cores.

Furthermore, to prevent the Gate's dedicated thread from monopolizing a CPU core and starving the worker threads, the Gate employs frequent, microscopic yields. By utilizing high-resolution timer interrupts—often on the scale of hundreds of microseconds—the Gate ensures preemptive execution, actively yielding the CPU to avoid polling overheads during brief idle periods, thereby naturally pipelining the processing of packets and bounding system latency.

### **4.2 Component 2: The Configurable Thread Pool**

While the logical actors provide the conceptual framework for concurrency, the Configurable Thread Pool provides the brute physical force required for computation. The Thread Pool is an explicitly bounded collective of physical operating system threads. By pre-allocating a stable set of worker threads at application startup and continuously reusing them to process millions of transient actors, the architecture entirely eradicates the immense computational penalties associated with continuous thread creation and destruction.

#### **4.2.1 Mathematical Thread Pool Sizing and Optimization**

The most critical configuration parameter in this architecture is the size of the thread pool. Determining the optimal number of threads is not arbitrary; it requires precise mathematical calibration based on the specific operational profile of the actors. If the pool is configured with too few threads, the system leaves available CPU cores idle, resulting in artificially constrained throughput and elevated latency. Conversely, if the pool is configured with too many threads, the operating system kernel is forced to aggressively context switch, leading to cache thrashing, wasted CPU cycles, and the potential for memory exhaustion.

The architecture adheres to a proven mathematical formulation for thread pool sizing, adapting the principles outlined by concurrency experts such as Brian Goetz. The optimal thread pool size (![][image1]) is determined by the equation:

![][image2]  
This formula provides a rigorous framework for configuration:

* ![][image3] represents the total number of physical or logical CPU cores accessible to the host operating system.  
* ![][image4] represents the target CPU utilization percentage, expressed as a decimal between 0 and 1\.  
* ![][image5] represents the average Wait time, which is the duration an actor spends suspended while waiting for external I/O operations, database queries, or network responses.  
* ![][image6] represents the average Compute time, which is the duration an actor actively utilizes the CPU to process mathematical or business logic.

The ratio of Wait time to Compute time (![][image7]) dictates the fundamental nature of the thread pool configuration:

**CPU-Bound Workloads:** In scenarios where actors are executing heavy mathematical algorithms, performing cryptographic operations, or processing in-memory data streams, the Wait time (![][image5]) is virtually zero. The formula simplifies drastically. For purely CPU-bound tasks, the optimal thread pool size is exactly equal to the number of available CPU cores (![][image8]). Adding even a single additional thread in a CPU-bound scenario will strictly degrade performance by introducing unnecessary context switching.

**I/O-Bound Workloads:** In contrast, if the logical actors frequently interface with external databases, perform heavy disk I/O, or issue synchronous HTTP requests to third-party APIs, the Wait time (![][image5]) significantly exceeds the Compute time (![][image6]). Under these conditions, the thread pool must be proportionally expanded. A pool serving highly latent I/O operations may require hundreds of threads to ensure that while a vast majority of the threads are blocked awaiting kernel I/O completions, a sufficient number of threads remain available to continue executing the unblocked actors residing in the Scheduler's run-queues.

#### **4.2.2 Heterogeneous Dispatchers and Thread Affinity**

To accommodate complex enterprise systems that exhibit both CPU-bound and I/O-bound characteristics simultaneously, the architecture supports the instantiation of multiple, distinct thread pools, commonly referred to as dispatchers. Developers can route specific classes of actors to specific dispatchers based on their workload profiles. For example, a parallel dispatcher bounded strictly to the CPU core count handles all numerical processing, while a boundedElastic dispatcher with a dynamically expanding thread count manages all database interactions.

Furthermore, the Thread Pool implements Non-Uniform Memory Access (NUMA) awareness to maximize hardware efficiency. The architecture utilizes operating system APIs, such as sched\_setaffinity in Linux environments, to physically pin specific worker threads to designated CPU cores. By binding a thread to a core, the system guarantees that the thread consistently utilizes the local L1 and L2 cache of that specific processor. When combined with the scheduling algorithms discussed below, this core affinity drastically reduces memory fetch latencies, as the state data of a frequently executed actor remains warm within the CPU cache hierarchy, entirely avoiding the slower main system RAM.

## **5\. Policies and Tactics**

This section describes the specific policies and algorithmic tactics employed to manage execution order and resource allocation.

### **5.1 The Pluggable Scheduler Subsystem: Algorithmic Dispatching**

When the Gate detects an event and notifies the system that a logical actor is in the READY state, the responsibility falls entirely upon the Scheduler to determine the absolute sequence and hardware placement of execution. In a system multiplexing millions of actors, a rudimentary First-In-First-Out (FIFO) queue is fundamentally inadequate for guaranteeing Quality of Service (QoS), meeting hard real-time deadlines, or preventing systemic starvation.

To address the diverse requirements of different application domains, the architecture features a highly decoupled, pluggable scheduling interface. Administrators and developers can transparently interchange sophisticated scheduling algorithms without modifying the underlying actor logic or the Gate's event loop. The system natively implements the forefront of concurrent scheduling algorithms: Adaptive Asynchronous Work-Stealing (A2WS), Earliest Deadline First (EDF), Priority-based Rate Monotonic (RM) scheduling, and Round Robin (RR).

### **5.2 Algorithm 1: Adaptive Asynchronous Work-Stealing (A2WS)**

Work-stealing is a highly revered scheduling paradigm designed explicitly for shared-memory parallel computation. In a standard work-stealing implementation, every individual worker thread within the Thread Pool maintains its own localized double-ended queue (deque) of tasks. When a thread requires work, it strictly pops an actor from the head of its own localized deque. However, when a thread exhausts its local queue and becomes idle, it transitions into a "thief" state. The idle thread randomly selects another busy thread (the "victim") and steals tasks from the tail of the victim's deque. This mechanism naturally and continuously balances the computational load across all available processors.

While conceptually elegant, traditional work-stealing suffers from catastrophic communication overhead in massively scaled, heterogeneous distributed systems. As the number of processors increases, idle threads generate immense memory contention and locking overhead as they aggressively poll and attempt to steal from active threads.

To rectify this limitation, the architecture implements a highly optimized evolution known as **Adaptive Asynchronous Work-Stealing (A2WS)**. A2WS completely overhauls the stealing mechanism by utilizing active telemetry gathering and deterministic mathematical targeting.

**The A2WS Execution Mechanism:**

1. **Limited Information Propagation:** Rather than threads blindly polling one another, the A2WS scheduler utilizes a bidirectional ring network to propagate localized performance telemetry. Each thread continuously updates a lock-free data structure with metrics regarding its current queue depth and processing latency.  
2. **Smart Stealing Equation:** When a thread becomes a thief, it consults the telemetry data instead of randomly selecting a victim. The algorithm executes a deterministic steal\_equation() to calculate an optimal *Steal Rate* (![][image9]), representing the precise number of actors that should be transferred to equalize the load.  
3. **Optimal Victim Selection:** The A2WS algorithm applies a select\_victim(S) function to parse the telemetry data and match the thief with the most mathematically optimal victim—typically the thread with the deepest queue and the highest latency disparity. This guarantees that a steal operation only occurs when it will yield a statistically significant performance benefit.  
4. **Asynchronous Theft Protocol:** Once a victim is selected, the actual transfer of actors is executed using fully asynchronous, non-blocking one-sided communications, often leveraging advanced atomic operations or Message Passing Interface (MPI) primitives. This asynchronous theft allows the redistribution of actors without requiring the victim thread to halt its execution or acquire blocking locks.

Empirical research demonstrates that A2WS yields a performance throughput improvement of approximately 10.1% over conventional state-of-the-art work-stealing implementations, making it the premier choice for highly variable, CPU-bound actor workloads. By keeping actors pinned to local deques whenever possible, A2WS maximizes L1/L2 cache locality, ensuring that the actor's state remains warm in the processor cache.

### **5.3 Algorithm 2: Earliest Deadline First (EDF)**

For operational domains constrained by inflexible temporal requirements—such as real-time embedded control systems, high-frequency trading engines, or telecommunications switching arrays—the architecture provides an Earliest Deadline First (EDF) scheduler.

EDF is a highly dynamic priority scheduling algorithm. Under this paradigm, the priority assigned to an actor is not statically configured by the developer; instead, the priority is calculated at runtime and is inversely proportional to the actor's absolute deadline. Whenever the Gate notifies the Scheduler of an event, the Scheduler computes the exact timestamp by which the actor must complete its execution. The actor is then inserted into a priority queue—typically implemented as a concurrent min-heap. The Thread Pool worker threads are mandated to always dequeue and execute the actor that is chronologically closest to its absolute deadline.

**Mathematical Optimality and Uniprocessor Proofs:**

The dominance of EDF in real-time systems stems from its mathematical optimality. Scheduling theory proves that if any set of periodic, independent tasks can be scheduled by *any* algorithm without missing a deadline, it can unequivocally be scheduled by EDF. The theoretical schedulability of a system under EDF is determined by the utilization bound test:

![][image10]  
In this equation, ![][image11] represents the worst-case execution compute time of actor ![][image12], and ![][image13] represents the period or the relative deadline interval. As long as the total combined CPU utilization requested by all actors remains below 100% (a value ![][image14]), the EDF algorithm mathematically guarantees that zero deadlines will be missed.

**Multiprocessor Anomalies and Partitioned EDF:**

While mathematically optimal on a single processor, extending EDF across a configurable thread pool with multiple physical cores introduces severe complexities known as "multiprocessor scheduling anomalies". In a naive Global EDF implementation, where all threads pull from a singular, centralized priority queue, actors may be continuously migrated from one core to another. This migration obliterates cache locality and introduces significant context-switching latency. Alarmingly, under Global EDF, increasing the size of the thread pool or the frequency of the CPU can paradoxically cause a previously stable system to miss deadlines.

To neutralize these anomalies, the architecture implements **Partitioned EDF**. The millions of logical actors are statically partitioned and assigned to specific worker threads within the pool. Each worker thread maintains an isolated, localized EDF priority queue. This entirely eliminates task migration, perfectly preserves cache locality, and allows the uniprocessor optimality of EDF to be independently validated and applied to every single thread in the pool.

**Overload Conditions and The Domino Effect:**

The primary vulnerability of EDF is its behavior during transient system overloads. If the CPU utilization exceeds 100%—even briefly—EDF suffers from the "domino effect". Because priorities are dynamically based on impending deadlines, multiple actors will converge on the same deadline simultaneously, resulting in a cascade of preemption and catastrophic deadline misses across the entire system. To combat this, the scheduler incorporates a safety protocol known as Controlled Preemptive EDF (CP-EDF), which actively suppresses excessive preemptions during overload events, thereby preserving processor cycles that would otherwise be wasted on context switching.

### **5.4 Algorithm 3: Rate Monotonic (RM) and Priority-Based Scheduling**

For mission-critical environments that prioritize absolute predictability over maximum CPU utilization, the Scheduler offers fixed-priority scheduling, utilizing the Rate Monotonic (RM) algorithm.

In strict contrast to the dynamic calculations of EDF, the RM algorithm assigns a static, immutable priority to each logical actor prior to the execution phase. The foundational axiom of RM scheduling is that priorities are assigned in direct accordance with the activation rate or periodicity of the actor's workload: the shorter the period (meaning a higher activation frequency), the higher the priority assigned. For instance, an actor tasked with polling a volatile hardware sensor every 2 milliseconds receives a statically higher priority than an actor designed to write aggregated log data to a disk every 500 milliseconds.

The primary limitation of the RM algorithm is its inability to utilize 100% of the available processor capacity while maintaining schedulability guarantees. The mathematical limitation for RM scheduling, famous established by Liu and Layland in 1973, is defined by the utilization bound:

![][image15]  
As the number of tasks (![][image16]) in the system approaches infinity, this utilization bound converges to ![][image17], or approximately 69.3%. If the total system utilization exceeds this 69.3% threshold, the RM scheduler cannot guarantee that all deadlines will be met, even if the CPU possesses significant idle capacity.

Despite leaving roughly 30% of the CPU underutilized in worst-case theoretical scenarios, RM is frequently chosen over EDF due to its superior resilience and predictability during system overloads. Because priorities are absolutely static, if an overload occurs, it is deterministically guaranteed that only the lowest-priority actors will miss their deadlines. The highest-priority, mission-critical actors remain entirely insulated from the overload, maintaining perfect temporal stability.

### **5.5 Algorithm 4: Round Robin (RR) and Temporal Slicing**

Round Robin (RR) serves as the most generalized, fundamentally fair scheduling algorithm available within the architecture, optimized for traditional, best-effort computing tasks.

Under the RR protocol, the Scheduler maintains a standard First-In-First-Out (FIFO) queue of all actors in the READY state. The Thread Pool selects actors in a strict sequential order. To ensure absolute fairness and prevent a long-running actor from monopolizing a physical thread, the Scheduler enforces a strict execution time quantum (a temporal slice) for each dispatch.

When an actor is dispatched to a thread, it executes its mailbox messages. If it successfully clears its mailbox within the assigned time quantum, it yields voluntarily and returns to the IDLE state. However, if the time quantum expires while the actor is still processing, the worker thread initiates a preemptive strike, suspends the actor's execution, and immediately re-inserts the actor at the absolute tail of the Scheduler's FIFO queue.

While Round Robin completely lacks the hard real-time execution guarantees provided by EDF or the structured predictability of RM, it absolutely eliminates the possibility of task starvation. Millions of heterogenous actors can share the underlying Thread Pool equitably. This algorithm excels in standard web server environments, microservice architectures, and data processing pipelines where actor computation times are highly variable and no single task is definitively more critical than another.

### **5.6 Comparative Algorithmic Analysis**

To synthesize the operational parameters of the pluggable Scheduler, the following table details the comparative metrics, mathematical constraints, and ideal use cases for each implemented algorithm:

| Algorithm | Priority Mechanism | Preemption Model | Max Guaranteed CPU Utilization | Optimal Use Case Profile | Fundamental Drawbacks |
| :---- | :---- | :---- | :---- | :---- | :---- |
| **A2WS (Work-Stealing)** | Load-balanced (No strict priority) | Non-preemptive / Voluntary yield | \~100% | High-performance parallel computing, Homogeneous tasks. | Complex victim selection logic; prone to cache thrashing if poorly configured. |
| **EDF (Earliest Deadline)** | Dynamic (Inversely proportional to absolute deadline) | Preemptive | 100% (Uniprocessor/Partitioned) | Real-time telecommunications, multimedia streaming. | "Domino effect" during transient overloads; high computational overhead for queue sorting. |
| **RM (Rate Monotonic)** | Static (Directly proportional to activation frequency) | Preemptive | ![][image18] (Liu & Layland bound) | Mission-critical embedded control systems, avionics. | Sub-optimal hardware utilization; inflexible to runtime task variations. |
| **Round Robin (RR)** | None (Strictly sequential) | Preemptive (Time-sliced quantum) | \~100% (Minus context switch latency) | General purpose web servers, best-effort HTTP traffic. | Zero Quality of Service (QoS) or real-time guarantees; poor response for critical spikes. |

## **6\. Detailed System Design**

This section details the module-level workflows and behavioral sequences.

### **6.1 System Workflows and Dynamic Inter-Component Interaction**

To fully appreciate the elegance of this software design architecture, it is necessary to trace the dynamic execution path of a single, highly concurrent event as it traverses the entirety of the system—moving from the raw physical hardware layer, up through the Gate, into the mathematical evaluation of the Scheduler, and finally concluding in the physical execution of the Thread Pool.

#### **6.1.1 Phase 1: Ingestion and Event Activation**

1. A hardware interrupt is generated by the Network Interface Card (NIC), signaling the arrival of a TCP packet containing a client request.  
2. The Gate, operating in an infinite, non-blocking asynchronous event loop leveraging epoll\_wait(), detects that the specific file descriptor associated with the socket is active and ready for reading.  
3. The Gate performs a non-blocking read() system call, rapidly extracting the raw byte payload from the kernel's memory space into the application's user space.  
4. The Gate applies protocol framing logic to parse the byte stream, identifying the target logical actor based on internal routing tables. It constructs a formal, immutable message object.  
5. The Gate acquires a lock-free reference to the target actor's MPSC mailbox and utilizes atomic Compare-And-Swap (CAS) instructions to safely enqueue the message without blocking.  
6. The Gate inspects the atomic status flag of the actor. Recognizing that the actor was previously in an IDLE state, the Gate immediately dispatches a high-priority notification to the Scheduler, passing the memory reference of the newly activated actor. If the actor was already READY or RUNNING, the Gate takes no further action, ensuring no duplicate scheduling occurs.

#### **6.1.2 Phase 2: Algorithmic Scheduling and Dispatch**

1. The Scheduler ingests the activation notification from the Gate.  
2. The Scheduler consults its active algorithmic configuration (e.g., Partitioned EDF).  
3. The Scheduler evaluates the actor. It reads metadata from the message payload to determine the required execution deadline, calculating the absolute temporal priority.  
4. The Scheduler inserts the actor into the appropriate internal data structure. In a Partitioned EDF configuration, it locks the specific min-heap assigned to a designated physical core and inserts the actor.  
5. If the system were instead utilizing A2WS, the Scheduler would place the actor into the local deque of the specific thread corresponding to the Gate's current core, relying entirely on the autonomous work-stealing telemetry to distribute the load if that specific core becomes saturated.

#### **6.1.3 Phase 3: Execution and Lifecycle Management**

1. An idle worker thread within the Configurable Thread Pool queries the Scheduler for its next task.  
2. The Scheduler pops the highest-priority actor from the queue (e.g., the actor with the earliest absolute deadline) and securely assigns it to the worker thread.  
3. The worker thread transitions the actor's internal state machine to RUNNING. It formally invokes the actor's defined behavioral function, passing the immutable message retrieved from the mailbox as an argument.  
4. The actor executes its business logic. Because it is securely isolated by the "shared-nothing" principle, it can safely mutate its own encapsulated internal state variables, write data to external storage, or construct a response message directed to another local actor.  
5. If the actor generates a new internal message, that message is sent directly back to the Gate, re-initiating the entire workflow cycle.  
6. The execution is strictly bounded by a predefined throughput limit or time quantum. Once the actor's mailbox is entirely empty, or the throughput limit is exhausted, the worker thread releases the actor.  
7. If messages remain in the mailbox, the actor is suspended and re-queued into the Scheduler as READY. If the mailbox is empty, the actor formally transitions back to IDLE, and the worker thread instantaneously requests the next pending actor from the Scheduler.

## **7\. Quality Attributes and Non-Functional Requirements**

This section details how the architecture satisfies non-functional requirements such as performance, reliability, and error recovery.

### **7.1 Advanced Performance, Fault Tolerance, and Resiliency**

When architecting a system expressly intended to scale to millions of active actors, localized optimizations are insufficient. The architecture must implement systemic, macro-level methodologies to ensure sustained high throughput, resilient error handling, and robust memory management.

#### **7.1.1 Cache Locality and the Memory Hierarchy**

In modern multi-core computer architectures, retrieving data from main system RAM is astronomically slower—often by two orders of magnitude—than reading data from the CPU's localized L1 or L2 caches. If millions of logical actors are scheduled haphazardly across random physical threads, the system will suffer from continuous cache invalidation, stalling the CPU execution pipelines as data is frantically fetched from RAM.

This architecture fiercely defends cache locality by leveraging the interplay between Thread Pool affinity and algorithmic scheduling. Because algorithms like Partitioned EDF and A2WS default to executing an actor on the same physical thread that previously processed it, the actor's state memory is highly likely to still reside warm within that specific CPU core's cache hierarchy. Furthermore, because actors strictly encapsulate their state and process messages sequentially, the data required for execution is tightly packed in contiguous memory blocks. This layout circumvents the pointer-chasing fragmentation common in complex object-oriented graphs, maximizing hardware cache line utilization.

#### **7.1.2 Supervisor Hierarchies and the "Let-It-Crash" Philosophy**

In an environment hosting millions of actors, individual software failures, unexpected data formats, or external API timeouts are statistically inevitable. If a logical actor encounters an unhandled exception during the execution of its behavior on a physical thread, the physical thread itself must not be allowed to crash, as that would permanently diminish the capacity of the Configurable Thread Pool.

The architecture adheres to the Erlang-inspired "let-it-crash" philosophy for fault tolerance. All actor execution functions are securely wrapped in isolated try-catch execution contexts. If an actor throws a fatal error, the worker thread intercepts the exception, completely isolating the failure to that specific actor instance. The system then automatically generates a failure notification and routes it to the actor's designated supervisor—typically the parent actor that originally spawned the failing child.

The supervisor actor evaluates the failure and applies a deterministic recovery policy: it may choose to seamlessly restart the child actor, completely clear its corrupted mailbox, or recursively escalate the failure up the hierarchy. This hierarchical fault tolerance guarantees that the destruction or corruption of a single logical actor has absolutely zero impact on the overarching stability of the Gate, the Scheduler, or the physical Thread Pool.

#### **7.1.3 Dynamic Backpressure and Overload Mitigation**

When the influx of network events at the Gate vastly exceeds the processing capability of the Thread Pool, the system enters an overload condition. Without robust systemic safeguards, the lock-free MPSC mailboxes of the logical actors will grow infinitely, eventually consuming all available system memory and resulting in a catastrophic OutOfMemoryError.

To prevent structural failure, the architecture implements dynamic backpressure mechanisms directly at the Gate layer. The Gate continuously monitors the global memory heap and the aggregate depth of all actor mailboxes. If a predefined critical threshold is breached, the Gate takes defensive action. For external network traffic, the Gate ceases to execute epoll\_wait() or read from the sockets. This deliberate halting leverages the underlying TCP sliding window protocol, naturally pushing the backpressure across the network and forcing the external client producers to slow their transmission rates.

For internal events generated by local actors, the Gate may proactively drop low-priority messages or dynamically route them to a specialized dead-letter queue for later analysis. By selectively shedding load and throttling ingestion at the absolute perimeter, the system ensures that the core scheduling engine and the Configurable Thread Pool maintain operational stability even under massive denial-of-service attacks or unprecedented flash-crowd scenarios.

[image1]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAD4AAAAYCAYAAACiNE5vAAACWElEQVR4Xu2XS6hOURTHl0fkJoSJ0s1AUpLUVQYGyoCZMqFERgyIGQNTA5JnUZRXYsBAUh55hpQxKY8MJCLPXO/n/2+t0/2fdb+jGy593z2/+vft9V9nd87eZ6999mdWU1PTl9gLvYG+h/aXss5X68pTs8rp5kYH1ogL0PhsNjv9oFPQMfOBzyunf1I1IU3NKqgj2lVv/Us2WoHn0n5lPvBh4k2ANkjcMugbZh0zvi3eYWioxC0B6/tE8vJyb7T0mx6tb/U42I0Rf5JcFdvN+4zOiV5knfk9p+dET3iRjaB465PMb5DhZjcyef9jZfz2Pas6njXP3YeGpBzJ/TgJ35L3L8jP0SMGQuezGfS37rVOnoqvua3mp74H5pO1UHKnoZvQDuiQlQ9Cl6Ez0EvxyF1oH7QU2iw+96TH0EnoAHQx5T5AR6Bd4pcYAD2DrueE8A56n03zB9mSPL7tTdHmhBaTwoGSIubvXGkPjvZsqC3ab63rK3IFWhttopPNvWemxE+kzWN2Nzgjr82/3/xuN7wITIGWZ9P8+l/VNzfH4xJPNP8/oKww73POfHLnhD8j/AK2B0X7IHQv5ZSH4d1J/l8j33B48jgxoyTmOWC1xIRLlQPJcInvkThPwqIUZ6aZl+PRnPhTdBmzbsl68/otKPLbJGY/ZZmVzw9ToXHmy3pxeAvMvx5jzPtzw22P3E7zMpgf8UdoZbSXWC/9g+yEbkjMkhkhMTcfrbeqc8Al6Jr5xsbBFbDGH0FjzVfP1fC5LzHmG+V+8BnaHbnJ5s9xC1oTXk1NTR/iB0m8m8IMosymAAAAAElFTkSuQmCC>

[image2]: <data:image/png;base64,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>

[image3]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACUAAAAYCAYAAAB9ejRwAAABlklEQVR4Xu2VOy8FURDHx6ORiHh0Ep1oRCh0CjdBp/IBfAGFTqMThVepUggNiQiFAhEqEVo0CgqRiHhHQiKE/5hZd3bsRuGeq9lf8kvOzGzunnvO7DlEGRlhmYVP8EOdi1WFd8rX2a54ORz2pUnswEafDEkJXIerJJPqi5e/SJtsMAZhu47TVuvNJ0Jza8YPJJOqMrkmOG7iomBXhvuG4xOTW4CVJg4O99Oay/ktTNrOoNh+sjmeyKTGr6ZWFO58QolWqxmOulpw0rZmi6R2BitcLSjlcNsnlVL62VuWKbgLD2EdnIYXsAYekexArT57TfEjZQy2mfibMngDD3zB8AxffBKcwl4d86S74TDJlbWsef6A7uGQeS4i8Y8uwUeS84nPJb7bkmiFAy7XQCk/SpKPjo4WjRnO/TqpvzABj31SsS9bgZs6nocjOu4g2eaCkoN7Jq6G/bCe0leDe6xTx/skq7+YLxcGvrg3SBo4p7kZkp7ixr8i+VAiuH+5N89Jtv8S9ph6MHhl7H357/DW8KT4w8gIyidsGmGj+dPpUQAAAABJRU5ErkJggg==>

[image4]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACMAAAAYCAYAAABwZEQ3AAABcUlEQVR4Xu2VPUvEQBCGRytPhGus/AmHlY0gWF7hr7DQRhQsFEGwEg7/g4WF2CpocQhWgqWCWPktoqCCcq1foO8ws2YcchHMbao88HKbdzY3s5tJlqik5H88QF9GH9CViS+6+KuJRaFKkujABwwcL4QVkmSjPmB48UYs3il75RPQjDdjEfqhHbdQlzdj0EtSyK4PGLIK7SjLJMmGfcDw6I1YvFH2ysehKW/G4q9+ufFGTLiQU28aeOcKg4u59qbCRQ54k+TNeoLWoTv1Lkn+axDahu7V74aOoAu9Ztr2YIPSH9MJVPcmSSF2Po+54B4dD6k/B81D52ZeIC3fD0skEz71lz+ClV8zEs6gNW8qNskmtKVjfmM3dMxf+Y71ISfs9ybJ7vAhG+B5I2YcFrcHTeo4N3xq95lrPteYVUp2gguzfZH2iI6Nl4tnaAc6pOSY4CSz0D7UVC8wBrVI+nCB5D7usWhkNmWRTJMUU/OBkrx8AwBOWicyyO1CAAAAAElFTkSuQmCC>

[image5]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABUAAAAYCAYAAAAVibZIAAAA+klEQVR4Xu2SMetBYRTGT7EaWJAPwIcgi8liN6AsJotPoP7FqnwEBhl9BqMyyGCRMlooEuF/jvfcOh3v9Vpkub964n1+PbfrXgAB3yCE2WMeIjt2GcxRuRU7YqtcWbgnJxY2vJGNBqalS485+A/fXfSqC8kYzDCp+grmzk7Tx8R0KemAGeZUf8ZM2YWVW6rzC3Uww5rohpgIZsAuLdxCfPclD2b4J7oZf7bZFfkcBfPTnaTADEd83ghXZdfk80U4JzSku0tguqLPsuthCpiScE5oeMDcVE//CHITi3NCQwrdjcZzcS1c0MjveZFb6/ITaEhv1ga5gIBf8A/vQUX4wN6bNQAAAABJRU5ErkJggg==>

[image6]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAA8AAAAYCAYAAAAlBadpAAAAsUlEQVR4XmNgGJZAGogLgHgmECshiVshsTHAYiD+D8S3gdgbiFWBeBoQPwdiS6gcVgCS+AfE/OgSQFDJAJG/hC4BAn8Y8JgKBSD5IHTBD1AJTnQJNIBhuC5U8Ba6BBaAofkvVBCbPwkCkEYME4kFZGtmZoBofIkugQVgtYAYmy2AOAFdEATuMkA0g1yBDYDEX6ELIgOQZlAiQTfACIhfo4lhBbsZEF74CqVTUVSMgqEIAG1gK0HBSgf2AAAAAElFTkSuQmCC>

[image7]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAC4AAAAYCAYAAACFms+HAAAB7klEQVR4Xu2WPSiFURzG/8VikFh8bSQWiwyI7oCFQRkNiBSLlAwGg1KsJGXGIKPRZDDKRzKQpJR8DAjJ9//vnNv938c5932V5NX91dO97/O897nnnve8571Eaf4/pWhEgUrWAJoOillDrHlWifLr1HvKYN2w3pWubFbBuofswGbCKWQdKnNxhgawQKbnkNXKKmPNkflcrc2+8ECegBIDc9HPGkbTwy0aCul/Y+VgwIySyXcxELbJP7hUA39Gw0MPqxFNywv5++NI3o6msEImLAS/k8xMuIpnWXloepAOF9dkurMwAFzf/8kkmbAB/EfWhs0yIduH41TsoEHmZpVefd/48A68l0zYrbwlVjZr0WblKttT74OYYRWhybyS6XWt69DEyJRMKG/Tvo7brMUe55JZJmHxzZb4viw0sndKybI9PlFZl80G7fGTyoKQ2ZRtzsWPDFyQEpnlAtaU8uttNs1qYrWpLIh1Ms8KRDzpPMfAQeCPkxNkr5W1p5GdRrJVRxZEqi8NM+M1lHzfOYkXyawi8SwfgxRUs0bQVByR6XRdEUH8CzRdSIlv/Up2jGYAYZeBPIBw8FWsS/C8SInsGC6CLqmLOzQ8rFHiisb/G/UlnfGLyBO3Gc0oIJc/kmyhEQXG6Hu7z58hhkaaqPMB1xqAJTwbFfkAAAAASUVORK5CYII=>

[image8]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAH0AAAAYCAYAAADXufLMAAADZ0lEQVR4Xu2ZR4hUQRCGyxwQFfUiBjyICCIiKHrwsKCgGFC8mFD0ogdFb6vgQRAP5oiCignRgx5EBANGVARBPJjADGLAnHOsf6vaqamdtzuzOzv7dukPfra6at5Md02/qn6zRJFIJBKJRBojO1kfWX9Vu7Oiwm/KxKHh2eFIFaQ6v/ZDc3GG1ds7I3mTuvw2YR1jHSKZ1MTscAVJk41UTyrzu4A1SO2k3fjLOyJ5k8r8vjb2O5JJtTe+PqwVZhwpjFTm1+489BWMbxvfflY7M44URuryi35zxPl8CcpVjhoT21l7E7SH5MSNU/gOfe2YiqvyI5X5tf3G+jCRVTr+YWJJbCS5posP1CHLSD5zqA+kiGLlt6i88Q4l7MZ+JMn14ODRyflKvmOpfj6zEGqa3zolKWknSWIPWG1cDPjrsAH+OF8p8POoCUtZKwvQOLksL5LmV11+64zmrNPeqTSlzG60vDB+G1tP0vsekSxkmokdZ91gbWbto+wfIc6zTrDeGh+4y9rFms1aa/zokc9YR0n67VkX+8Y6wNpq/PVFTfIbWM26yLrG6syawrrM6s+6SpIfVAnwkPVBbTCBpIVUohnrFckbJfGF9dU7Sb6Edc6Hu3yN2lhsWAy+ZBDG+Dve2K3UHslqq/ZnypxmL7AWqw1sktALy8z4ubHx02Z9Upv83meNVRvrHUFSGWaw7oUXaWyI2lhva7Xxvl3V/g/uhPckz494bkxK0ADWXO8keX1V/Ry77LAZ9yX5/dkyj+SaUyQLH6X+YeoPwG6pNk7TftGWx+q74/ylpjb57UGV1xXAunBzBHyectlFw79pB+fDIlGSAngOLTdjgPKML9GDsoVHo4BfzHQ39gwmaUEHfaCBgDMDWmEu7Hrnk1QSMIukTYIWlDsvtcKW7jC55ST9OhDiG8wY11nmUPbz60BWL5JSjjIGJpM8JaBU4XqUuJ4a20JS+ifp+DtJIsBMKuF/qopMGeuSGXekTD78DRDYxFqiNjYNNjzyW1Q+sa6bMcoYJhfAQcv216Tn0HMkC8QhzvYg9PSnrO4kVQMHGoA+iTHuZPT/n6xtGsMBB/O4xVqovoYK/jGDw+9LypxZprKusJ6QtENUVwvu+pusbiTxRdnhSEMEh7vR3hlpvOAXPZRztMBIJBJJ4B+I0At9l9A9pQAAAABJRU5ErkJggg==>

[image9]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAA0AAAAYCAYAAAAh8HdUAAAAqklEQVR4XmNgGLJADYhnArEvklgJEhsFsALxPyCeDcR8QGwHxP+BuAaIPyOpQwEgBTboggwQ8Sp0QRBYwACRxAZA4iBXYACQBD5NWAFMUy+6BD7QzYDQCMMzUFTgAHkMmBpvoaggAFwY8PuTIRhdAAoWM+DQ5AfEBeiCUFDKgEPTWSBehy4IBX8ZcAQGzN08aOJrGfAknSdAzATEHxggmt9D6QVIakbBwAEAIrItoSGpzDcAAAAASUVORK5CYII=>

[image10]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAmwAAABJCAYAAACAa3qJAAAEHklEQVR4Xu3dXahlYxgH8BdpiiKjJNwwSUSDEW7UuZF81LgV5UY+pilC5MooH8UFRcaFK3KJ5sqFknJhXMzgRoSaC0SiMfIx5et9W3vZaz/2WXut2efss7bz+9W/fd7nWXvvc+6e1jrvWikBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAADN6HOVfkHMz5JefsyTYAABvp8ZzXcp4YrZ/M2TtuAwAwBH/nnDj6+c+crY0eAAADUAa2WhnYTmis5/VHqj7/6Zzzc17M2ZZzpHkQAADtnm38fDTnrcZ6HmVQeykWU1VvDokAAGyA+szaNK+n1XsAACzAG6l9INud81wsAgCwOGVYuzcWGy7J2RKLAAAsxqup/ewaAAAb7OfUf2A7HAsAAJvNd2m8M/Py0JvmuJwdOffk/JbG7y0plzPbfJX6D2wAAKTJoetYfJqq9/4aG8FKav+OL8J6JawBADat29J4YDsQen18HQtT3Jeq23rEG/B+H9blBrrld9kZ6gAAm9bnaTy03Rh6fVwYC1O8narvKQ+TL68fTLb/1XY2DgBgU5r30uhauj7ns1hckLKbtcvgCQCwIeqB7VCoL9q3o9d9E9X+yiaJ8vc8EhvBOTkf5+xK1fEXTbYBAIZjfxoPbXeG3qJ12bXa5lDOJ7E4Q/lOAxsAMHgvp+FcGu3rhZy/cq6OjY4MbADA0qgHth9iY6CuTNWgdl5s9GRgAwCWxulpPLSV23AM3dacH3Oui42e6oHt4tgAABiiR9N4aNsz2eqt/px50sVdqTr29tjoqB7YZj21AQBgMMrwUu/YXCY3peqxW6fGxgz1wLY9NgAAhujmnLtjcQkdTdVGii7qge2y2AAAGJpzU/fLkMvgtJxTYnGKG1L1d5czdAAAg1VuNvtNLK6hvS15Puep8aEAAEyz3mfWyuc/kKrLjteM1is5F+TcMVoDALCKeYelskuzzcGwfjP99zufCesuruoQAICl937qv6uy6bFYmKLc3LZp2i073gtrAABSNTRdG4sdld2k0wavWY5P1XsOxAYAAJN+T9VNcvu6P1VnzOph7aPJ9kwPJjsyAQBmaj6Cat6ckfo5kqr3AQDQU7mtR0m5ZFmnrNdaGdZWe07pjpyzYhEAgMUqA9ux7AgFAGABtqTVL4f+lFbvAQCwzl7J2Z/G//f2Zc67zQOybcnABgAweLfEAgAAlX2x0OLknMOxuAb2jF7faRYBAOiv7Bxdr0uXfW8RAgCwKeyMhRnWc2ADACAoOzPPbKwfbknNwAYAsGDlEVV9GNgAABasOXw91JKagQ0AYIF25+xK1SOhujgp59ZUDWzbQw8AgHVyaSwAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAP93/wBuZ+OXqkIptAAAAABJRU5ErkJggg==>

[image11]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABMAAAAZCAYAAADTyxWqAAAA6ElEQVR4XmNgGAXUBO1A/B8Ni0LlPsEUEQIhDBCNT9AlgOAzEH8E4n/oEthAKgPEIBd0CSjgY4DIT0KXQAd2DBCFC9El0ABIjTC6IDqAhQshQFDNSgaIonx0CSwgFl0AHRDrKqIAJYYxogtQYtgHZA4zA8Sgl8iCOAC6hSBXBaOJEeUyCyBOQBfEBu4yQAwDuRIbAIm/QhPbBsRvgZgFTRwMQIb9YcA00AiIX6OJiQGxMhCfBWJ/NDk42M2A8PJXKA3KYrgAoaAhGngC8U10QXLBDyCWBeJ96BLkgEIgPgXEMugSo4B4AAA0lTZL1T4Z1QAAAABJRU5ErkJggg==>

[image12]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAcAAAAXCAYAAADHhFVIAAAAaElEQVR4XmNgGHigAMT30QVh4C0Q/0cXpAx0AnECuiAI/IDSIPsckSVmAjETlA2SdEWSY6iF0v0MeFwKkihEFwSBPAaELmEgNkGSA0u8g7IfI0uAwDMgPsQAsT8TTQ4MAoBYDF1w6AAA4oAS3/pLqloAAAAASUVORK5CYII=>

[image13]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAABEAAAAYCAYAAAAcYhYyAAAAzklEQVR4XmNgGAW4wGYg/k8CxgpAEmFYxNA1aGARAwMhBohLkAETA0TxBTRxEHiELgACW4GYEU2sgAFiiD+aOBsQ96GJgUE+ugAQvGfA7mwBIBZHF8QFsIUHSYCZAWLAGXQJUkA5A8QQb3QJJLAAiG3RBZHBZwbCXqlGF0AHFIcHKArxhQc7EJ8H4pvoEshgNgPEkAQ0cRj4AqUxXBoExN8YIGnjLRSDwuUXAxbFQNAJxDPQBUkF2AwmCXAA8T8o2w5ZglTwgwESuKMADwAAQIQ3kXBCoT0AAAAASUVORK5CYII=>

[image14]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAB4AAAAXCAYAAAAcP/9qAAAA0klEQVR4Xu2UMQ4BYRCFR+ICEp0OjRMQhUSto5M4ATqNXpxA6Q6O4Bp6EZ2CRFDgxeyfyBO7/+xuIpL9kq+ZN5PXjUhGxic5HkSQhzceWmjDB5xx8IWN6L7TzED0cMSBJ0cxFk9FD7ocGPEuXsA7bHAQk8jiFbzAMgcJCS0ei4ZNDlIgtNgxEV3qcZAAr2JHX3R5yEEMTMWOlujRnAMDsYodVXiFSw48SFTsKMA1DyM4SwrFFk5wD7eBO3iAlfeln1GCHU/T+mgvirDuaS24yfgPns1INeKAKp1eAAAAAElFTkSuQmCC>

[image15]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAmwAAAAvCAYAAABexpbOAAADYElEQVR4Xu3dy8ttcxgH8F/u5TZwOQOX/0AUKQM5JZyOWxIjykRkYIooZWBwckmZyMDAgJE6A7mHQm4JJRmIgYTklolcf4+1F7/3aa1tp7PO3u+7Pp/69r7P81vr3b+938nT3mvvXQoAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAFQn5wYAAAff27kBAMBmuK/myjI+sJ2dGwAAHBhP58Z/GBvYPmp+v27x85Sac5r+Olxbc1tuAgDz9EbNK4u83vTPrHm1dINR/DyxWVu3sWHt9NxoDA1sx9ec1tRxLdv3NYfW7K75sFmb2tDj+05uAADzdFjNnzU35YXq0dKtHZsX1uiams9T7+aa32o+Kd1+r9+6/Ld3c6N0g2jvqpoba15b1BfXfPzv8mRiaPy95pu8sPBWbgAA83NJ6YacIU/UnJWba5b3Gi8bvl9zSNOLYx5o6vBeqsOlqY7zjmh+P7xZm1Lc1tjA9kfN1bkJAMxLvCSah6DeWH+dfk71m6Xb58NNL+q89xjqWg+lOrTn5POntGxgizdN/JKbAMC8DA03vbEh4kC7vXQvCz64qH8s3bVkj/xzRCcuxN+bekPi/nyam0ncZhYvD/fimbajmnpKywa2MPb/AQBmIoaBZ3Jz4ZbcGPDZijmpP2HAFzV3lW4v3zX9PKjsL8MX57dOKN15R+aFDWZgAwCWimHg3Nw8iOI6svNLd4F/Hkxy/UGqh8SbDy7IzWTTvtkg7ue3udnIjwMAMCO7y/gw8FhuTCz2cW9Tn1fzdVOHeKZumfgb7cd0TO2KFbKK/MxiNvY/AgBmIN5hOTYMjPWzfSvmuP6EEXF7xzT1V6Ub2lrPlu4z0obcWnN0U6+6/00Qe/0hNxvb6b4AABMYGgaGelPLt9nXLzS9uKbuoqbuxRsU7qm5s+bumidrntpyxGaL+/pTbjbyYwMAzEwMA/0n+l9Ylg8OU7ms5sXUi33tKd1nwbWeS/XzpTs254bmmE31Uum+XaLf88s1d2w5opTLF2sAANvG3IaXX2vOyE0AgE32ZRn+Gq2dam4DKgCwA8TXRc1liNlVc2puAgBsF4/nxg4Tb664PzcBAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA/oe/ALCupwrNIujFAAAAAElFTkSuQmCC>

[image16]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAwAAAAXCAYAAAA/ZK6/AAAAlUlEQVR4XmNgGAWDCegC8Twg5obyeYG4AYgnADETVAwO2IF4KxBHA/F/IG4G4gVQuXqoGArYC6VhGhqR5EA2YWgohdLXGDAls7GIwQFIoh2L2GU0MTCQYIBIgpwAA3xQMQUofypCCsJBtxpZrBqIlZDkGP4C8VdkASAoYIBo0AfiS2hyDBZAzIouyACJHwN0wVFACAAA3qgdBAlcrcAAAAAASUVORK5CYII=>

[image17]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAACkAAAAXCAYAAACWEGYrAAAB9ElEQVR4Xu2WSytFURiGPzJxSblPUMhtIiMz/AIRKQMzA5mYycRUSinKbWamTI1koGTgB8jA6Lgll8SEmOB7z9ory7vX2WcfB1GeeuvsZ6397W/f1j4i//xu+likoZ1FKupZfJIjTTnLGLyycLkVMyFyUkxmNFMslVrNsZhjHGiKP4wa6jR3LF1QJNsmc8VfY0Cz72wnxMzrdZzlQdPD0lIp/gNkwrZmgaWYuoMe5zteo/h9klKJGIwJ9s9jKf6GbgLXSh7AF7AEJfKxUItmXLOo6Qwcrsa6ps1OcsBLx41YxjRD5J7FzPc9m/CzLAE32a3ZDNyoZi/w9nbw7UPRVE368F1dCx4bnEQIbtLiK4ZtPOAuu4GPw5KYuc08ELAqKWpFNbnmcTz3JEg6ysTs28EDDpMSrp8kqsllj+O5CTHrYBQ5Yvar4QFiQsL1k0Q1ycuKr8ktzQs5BvsUOtsjmgZn27Ii4fpJqsQM4Gxd4PCGs+MiOBF2Lk8SXp4OaduCE35kCbBe4SD8zYXb8DhuqMnjLKfyvg/HB/w8S7zuF5ozzaWYb/lc8BvuXHOvqRCzCFuH3y4ojk8jw43FaRKP37dwrZlmmSHVkrr5LyFfsj8A/gX1s/xqdsR8Bj8DXl48Yj/ClaaIZQyyvQsZM8wiDV0s/hxv8USP4LEbeQgAAAAASUVORK5CYII=>

[image18]: <data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAEcAAAAXCAYAAABZPlLoAAAC9klEQVR4Xu2XSchOURjH/2QWypDI8JEFKZEhC1EkKTKUjQ1KlPqyUZSNqSRKWJn6FpKFhVJixUqGBcmYQhGRUMg8PX/POd/7vM977kB5E/dX/+59/ufce8859znnngtUVFRks0DUwZuOmd741+kl+i6aIHouWlpf3M5J0R5vNoONoq9Bc1wZGSS6Ae3EIVdWxD7odbz3TldGPopWmJh1qe2i2aJtIf5m6jSNz6KD4bwrtBHdasVYDW1cxxCvRPmGvhe1mDh23MK4p4sjg8PxrqiH8ZvCC2gqR5i2bNyUEHNAGPt0pseMyKMLtN4b450IHgc84geLGWaZhsbnZzJGNNSbCYoWtxHQhg1z/jhzvgRax68DqQxIwTqXTXw6ePONx3iAiy1+sJK0QC9kSr8L56tsBUN30SRvOi6g1hC+Zc5vTxu0zkLnsw2+E2VIDepD0YFw3l901JTdR8nplJrn56BrxhDnc64XERt6TDRDNDbENoWXBe93M8fCrw2v6eMLoH07jvosmSXabeJc5noj0A9609hgivO0iFh3s/FGBs8ONuO9Jo5e2cGZCl2fbouuQLO0DHag+oqeiq4Z75fp5I0csjpI74mJNwQvckr0wXllOQu9jtMnD041Lg0R+yzOlCQ7UOsU0zCP6d5w5A2O97lBuyQ6D32LqTplmIjiazlD7H7oiOi1ibnPavjYXBRtNfF+6EMWGy+yHsXpy86mGlnUeMLyR950rIHW4w7YUnT/Ly5mlt40MT8OcavRzi5vBK5DH7YO+n/CneyduhppRiPdSHptLn5l4s7BG2g80or6FxIHYZPxRhk/xWPoRtTyDPWDs0g02cQ/ycuE3tAO3YLuYMvCLcEZE/M3wjec8QMTv4RmnWU8Gju9FrqIWuIvyHDnE+59tngTupS8NfFhJKbVn4LTgw3+BP2kxt+EyHJo+b1wzPq34mZvnvPifxF34Tzy/lkbWP5jZcFr44Cwnf8VXD/9S7FwZnBRvuoLKioqKir+En4ASGnO3MDDH0gAAAAASUVORK5CYII=>