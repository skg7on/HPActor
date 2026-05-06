# Actor System CLI Interactive — Core Concept and Architecture Design

## 1. Executive Summary

This document specifies an interactive CLI subsystem for introspecting, debugging, and managing a running HPActor system hosting millions of actors. The CLI uses a **Hierarchical Command Tree** for multi-level command dispatching and an **Asynchronous Request-Response** model for thread-safe actor inspection. A dedicated **CLI Actor** runs on its own OS thread, blocking on `stdin` without disrupting compute workers.

**Key Design Decisions:**
- **Trie-based Command Registry**: Multi-level commands (`/actor 0x123 show`, `/system stats`) are dispatched through a tree of command nodes, not a flat `if-else` block. This yields natural tab-completion, per-node help text, and extensibility without modifying a central dispatch table.
- **Inspect Message pattern**: The CLI never reads another actor's memory directly. It sends an `InspectStateRequest` system message; the target actor handles it on its own thread, serializes internal state, and replies with an `InspectStateReply`. This eliminates race conditions and memory safety concerns inherent in direct inspection.
- **Dedicated I/O thread**: `std::getline(stdin)` is a blocking operation. The CLI Actor runs on a dedicated OS thread (via `DaemonActor` or `BlockingActor` semantics), isolating blocking I/O from compute workers.
- **Paged/streamed list output**: For systems with millions of actors, commands like `/actor list` cannot materialize the full list. The system Registry (already sharded) provides a cursor-based paging API, and the CLI renders 50 actors at a time.
- **Virtual `to_metadata()` interface**: Every actor exposes a `to_metadata()` method returning a lightweight, publicly-inspectable summary. The CLI formats this generically — no knowledge of specific actor types required.

---

## 2. Architecture Overview

```
┌──────────────────────────────────────────────────────────────────┐
│                        Terminal / REPL                            │
│  $ hpactor attach --socket /tmp/hpactor/system.sock               │
│  hpactor> /actor 5 show                                           │
│  hpactor> /actor 5 kill                                           │
│  hpactor> /system stats                                           │
└────────────────────────────────┬─────────────────────────────────┘
                                 │ stdin/stdout (local) or
                                 │ UDS/TCP socket (remote attach)
                                 ▼
┌──────────────────────────────────────────────────────────────────┐
│                        CLI Actor                                    │
│  ┌──────────────┐   ┌──────────────┐   ┌────────────────────┐    │
│  │ Input Thread │   │ Command Tree │   │ Output Formatter   │    │
│  │ (dedicated)  │   │ (Trie-based) │   │ (JSON, pretty,     │    │
│  │              │   │              │   │  tabular, yaml)    │    │
│  │ blocks on    │   │ /actor       │   │                    │    │
│  │ stdin/socket │   │  └─ <id>     │   │ stdout or reply    │    │
│  │              │   │      ├─ show │   │                    │    │
│  │ lexer/token  │   │      ├─ kill │   │                    │    │
│  │ parser       │   │      └─ dump │   │                    │    │
│  └──────┬───────┘   │ /system      │   └────────▲───────────┘    │
│         │           │  ├─ list     │            │                 │
│         │           │  ├─ stats    │            │                 │
│         └───────────┤  └─ config   │            │                 │
│                     └──────┬───────┘            │                 │
│                            │ command execution   │                 │
│                            ▼                     │                 │
│              ┌──────────────────────┐            │                 │
│              │ Request Dispatcher   │────────────┘                 │
│              │ (sends InspectState, │                              │
│              │  KillRequest, etc.)  │                              │
│              └──────────────────────┘                              │
└──────────────────────────────────────────────────────────────────┘
                                 │ sends TypedMessage to:
          ┌──────────────────────┼──────────────────────┐
          ▼                      ▼                      ▼
   ┌──────────────┐   ┌──────────────┐   ┌──────────────────┐
   │ Target Actor │   │ ActorSystem  │   │ MetricsActor     │
   │ (any type)   │   │ (Registry)   │   │ (existing)       │
   │              │   │              │   │                   │
   │ handles      │   │ enumerates   │   │ serves stats     │
   │ InspectState │   │ actors in    │   │ from ring buffer │
   │ on own       │   │ shards       │   │                   │
   │ thread       │   │              │   │                   │
   └──────────────┘   └──────────────┘   └──────────────────┘
```

### Ownership Chain

```
ActorSystem
  ├── owns CliActor (spawned as system actor, before topology)
  │     ├── owns CommandTree (root node + children)
  │     ├── owns InputThread (dedicated thread, blocks on stdin/socket)
  │     └── owns OutputFormatter (JSON, pretty, tabular renderers)
  └── ActorContext (per-actor)
        └── holds CliActor* (set during spawn, null for non-CLI actors)
```

---

## 3. Command Tree Design

### 3.1 Trie-Based Command Registry

Every node in the tree is a keyword. Leaf nodes carry execution logic (a `std::function`). This avoids a monolithic `switch` block and allows auto-completion and contextual help.

```
/ (Root)
├── actor
│   ├── <id>              → Parsed as ActorId, stored in context
│   │   ├── show          → ExecuteActorShow(ctx)
│   │   ├── kill          → ExecuteActorKill(ctx)
│   │   ├── dump          → ExecuteActorDump(ctx)
│   │   ├── trace         → ExecuteActorTrace(ctx)
│   │   ├── link          → ExecuteActorLink(ctx)
│   │   └── mailbox       → ExecuteActorMailbox(ctx)
│   └── list              → ExecuteActorList(ctx)   [paged]
├── system
│   ├── list              → ExecuteSystemList(ctx)
│   ├── stats             → ExecuteSystemStats(ctx)
│   ├── config            → ExecuteSystemConfig(ctx)
│   ├── threads           → ExecuteSystemThreads(ctx)
│   └── memory            → ExecuteSystemMemory(ctx)
├── metrics
│   ├── show              → ExecuteMetricsShow(ctx)
│   └── watch             → ExecuteMetricsWatch(ctx) [streaming]
├── topology
│   ├── show              → ExecuteTopologyShow(ctx)
│   ├── graph             → ExecuteTopologyGraph(ctx)
│   └── restart           → ExecuteTopologyRestart(ctx)
└── monitor
    ├── start <filter>    → ExecuteMonitorStart(ctx)
    ├── stop              → ExecuteMonitorStop(ctx)
    └── log               → ExecuteMonitorLog(ctx)
```

### 3.2 Command Node API

```cpp
struct CommandNode {
    std::string keyword;                                           // e.g., "actor", "show"
    std::string help_text;                                         // one-line description
    std::optional<std::function<result<void>(CommandContext&)>> execute;  // leaf action
    std::vector<std::unique_ptr<CommandNode>> children;            // sub-commands
    bool is_parameter = false;                                     // true for <id>, <filter>, etc.

    // Register a sub-command. Returns the child node for chaining.
    CommandNode* add_child(std::string keyword, std::string help);

    // Register a leaf action on this node.
    void on_execute(std::function<result<void>(CommandContext&)> fn);
};
```

### 3.3 Command Context

Passed to every leaf execution function, carries everything needed to produce output:

```cpp
struct CommandContext {
    std::vector<std::string> args;            // remaining unparsed tokens
    std::map<std::string, std::string> params; // named parameters: {id: "0x123", filter: "Worker"}
    ActorSystem* system;                       // access to registry, scheduler, config
    CliActor* cli_actor;                       // for sending messages + streaming output
    OutputFormatter* output;                    // selected formatter (pretty, json, yaml)
    bool paged = false;                        // true if output should be paged
    uint32_t page_size = 50;                   // items per page for list commands
};
```

---

## 4. The Inspect Message Pattern

### 4.1 Problem

Direct memory reads of actor state from the CLI thread are unsafe:
- The target actor may be running on another thread, mutating its state.
- Lock-free data structures don't protect against partial reads.
- Even with locks, the CLI would contend with the hot path.

### 4.2 Solution: `InspectStateRequest` / `InspectStateReply`

The CLI Actor sends a message. The target actor handles it on its own thread, at a point where its state is quiescent.

```
CLI Actor                    Target Actor (Worker Thread)
    │                              │
    │  InspectStateRequest         │
    │─────────────────────────────▶│  (arrives in mailbox)
    │                              │  (actor processes on its thread)
    │                              │  calls to_metadata()
    │                              │  serializes to protobuf
    │  InspectStateReply           │
    │◀─────────────────────────────│  (sent back to CLI)
    │                              │
    │  formats and prints to stdout
```

### 4.3 Message Definitions

```protobuf
// CLI system messages
message InspectStateRequest {
    uint64 target_actor_id = 1;         // actor to inspect
    repeated string fields = 2;         // optional: specific fields to return
    bool include_state = 3;             // include actor-specific state blob
    bool include_mailbox = 4;           // include mailbox depth + stats
    bool include_children = 5;          // include child actor list
}

message InspectStateReply {
    uint64 actor_id = 1;
    ActorMetadata metadata = 2;         // common fields (all actors)
    bytes state_blob = 3;               // opaque, actor-type-specific (optional)
    MailboxSnapshot mailbox = 4;        // queue depth, messages waiting
    repeated ChildInfo children = 5;    // child actors
}

message ActorMetadata {
    uint64 actor_id = 1;
    string actor_type = 2;              // type_name()
    string state = 3;                   // "Running", "Idle", "IOWaiting", "Terminated"
    uint64 incarnation = 4;
    uint64 messages_processed = 5;
    uint64 uptime_ms = 6;
    string behavior_name = 7;           // configured behavior name (TOML topology)
}

message MailboxSnapshot {
    uint32 depth = 1;
    uint64 total_enqueued = 2;
    uint64 total_dequeued = 3;
    uint64 max_depth = 4;
}

message ChildInfo {
    uint64 actor_id = 1;
    string actor_type = 2;
    string state = 3;
}
```

### 4.4 `to_metadata()` Virtual Interface

Every actor exposes a lightweight summary via a virtual method on `AbstractActor`:

```cpp
class AbstractActor {
public:
    // Returns common metadata + optional type-specific state blob.
    // Called from the actor's own thread — safe to read internal state.
    virtual ActorMetadata to_metadata() const {
        return ActorMetadata{
            .actor_id = id(),
            .actor_type = type_name(),
            .state = actor_state::to_string(state_.load()),
            .incarnation = incarnation_,
            .messages_processed = messages_processed_.load(),
        };
    }

    // Override to provide type-specific state as a serialized protobuf blob.
    // Default returns empty.
    virtual bytes serialize_state() const { return {}; }
};
```

Actor types override `to_metadata()` and `serialize_state()` to expose relevant internals:

```cpp
class MyWorkerActor : public event_based_actor {
    bytes serialize_state() const override {
        MyWorkerState state;
        state.set_requests_pending(requests_pending_);
        state.set_average_latency_us(avg_latency_us_);
        return state.SerializeAsString();
    }
};
```

---

## 5. Paged/Streamed List Output

### 5.1 Registry Cursor API

For the `/actor list` command, materializing all actors at once is infeasible. The existing `ActorRegistry` provides a sharded, cursor-based enumeration API:

```cpp
class ActorRegistry {
public:
    struct Cursor {
        uint32_t shard_index = 0;
        uint32_t offset = 0;
    };

    // Enumerate up to `limit` actors starting from `cursor`.
    // Returns the next cursor, or nullopt if no more actors.
    struct PageResult {
        std::vector<ActorMetadata> actors;
        std::optional<Cursor> next_cursor;
    };

    PageResult enumerate(Cursor cursor, uint32_t limit);
};
```

### 5.2 CLI Paging Behavior

```
hpactor> /actor list
── Actors (page 1, 50 total shown) ──────────────────────────
 ID      Type              State        Uptime    Mailbox
 0x0001  EchoActor         Running      12m 03s   0
 0x0002  WorkerActor       Running      12m 02s   3
 0x0003  WorkerActor       Idle         12m 01s   0
 ...
 0x0032  SupervisorActor   Running      11m 58s   0
── Press Enter for next page, 'q' to quit, '/find <term>' to search ──

hpactor> /actor list --format json --no-pager
{ "actors": [{ "id": 1, "type": "EchoActor", ... }], "next_cursor": "..." }
```

The `--no-pager` flag disables interactive paging. Machine-readable formats (`--format json`, `--format yaml`) default to `--no-pager`.

---

## 6. Input Processing

### 6.1 Lexer → Tokenizer → Command Tree Traversal

```
User input:  /actor 0x123 show --detail --format json

Lexer produces tokens:
  [/] [actor] [0x123] [show] [--detail] [--format] [json]

Parser walks command tree:
  1. Root node matches "/" (optional, auto-inserted)
  2. "actor" → traverse to actor node
  3. "0x123" → parameter node <id>, store in context.params["id"]
  4. "show" → traverse to show node (leaf)
  5. "--detail" → flag, store in context.params["detail"] = "true"
  6. "--format" → option with argument, store in context.params["format"] = "json"

Execute node "show" with populated CommandContext.
```

### 6.2 Lexer Rules

- Tokens are whitespace-separated.
- Parameter nodes (`<id>`, `<filter>`) match any non-keyword token.
- Flags start with `--` (e.g., `--detail`, `--no-pager`).
- Options are `--key value` pairs (e.g., `--format json`).
- Integer parameters accept hex (`0x123`), decimal (`291`), or named (`echo-actor-1` → registry lookup).

---

## 7. CLI Actor Implementation

### 7.1 Thread Model

The CLI Actor uses a dedicated I/O thread for blocking input:

```
Thread 1 (CLI I/O):    blocks on stdin/socket read
                       │
                       ▼ tokenized input
Thread 2 (CLI Logic):  CommandTree traversal + message dispatch
                       │
                       ▼ InspectStateRequest via ActorContext::send()
Target Thread (any):   handles InspectStateRequest on its own mailbox thread
                       │
                       ▼ InspectStateReply
Thread 2 (CLI Logic):  formats output, writes to stdout
```

The I/O thread never touches actor state directly. It parses input and enqueues a lightweight token list to the CLI Logic thread (or, for simplicity, the CLI Actor could be a single-threaded `BlockingActor` that blocks on stdin in its `run_once()` loop).

### 7.2 Remote Attach

The CLI Actor listens on a UDS socket for remote attachment:

```cpp
struct CliConfig {
    bool enabled = true;
    std::string listen_path = "";              // UDS path, empty = stdin/stdout only
    uint16_t tcp_port = 0;                     // optional TCP port (0 = disabled)
    std::string default_format = "pretty";     // pretty, json, yaml
    uint32_t page_size = 50;
};
```

When `listen_path` is set, the CLI Actor creates a UDS listener. Remote clients (`hpactor attach --socket /tmp/hpactor/system.sock`) connect and interact identically to local stdin/stdout mode.

### 7.3 Actor System Integration

```cpp
class ActorSystem {
public:
    // CliConfig is part of SystemConfig
    struct Config {
        // ... existing fields ...
        CliConfig cli;
    };

    // Spawned during ActorSystem construction, before topology actors.
    // The CLI actor is a system actor — it cannot be killed by user commands.
    void init_cli();

    CliActor* cli_actor() { return cli_actor_.get(); }

private:
    std::unique_ptr<CliActor> cli_actor_;
};
```

---

## 8. Output Formatting

### 8.1 Formatter Interface

```cpp
class OutputFormatter {
public:
    virtual ~OutputFormatter() = default;

    virtual void header(const std::string& title) = 0;
    virtual void table(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) = 0;
    virtual void key_value(const std::map<std::string, std::string>& pairs) = 0;
    virtual void tree(const TreeNode& root) = 0;
    virtual void raw(const std::string& text) = 0;
    virtual void error(const std::string& message) = 0;
    virtual std::string finalize() = 0;  // returns complete formatted output
};
```

### 8.2 Built-in Formatters

| Formatter | Description |
|-----------|-------------|
| `PrettyFormatter` | Box-drawing characters, ANSI color, responsive column widths |
| `JsonFormatter` | Machine-readable JSON, one object per result |
| `YamlFormatter` | Machine-readable YAML |
| `TabularFormatter` | Whitespace-aligned columns, no ANSI, suitable for `grep`/`awk` |

### 8.3 Pretty Output Example

```
hpactor> /actor 5 show
┌────────────────────────────────────────────────────┐
│ Actor 0x0005 — WorkerActor                         │
├──────────────┬─────────────────────────────────────┤
│ State        │ Running                             │
│ Incarnation  │ 1                                   │
│ Uptime       │ 12m 03s                             │
│ Processed    │ 8,421 messages                      │
│ Mailbox      │ 3 queued, 0 high-priority            │
│ Children     │ 2 (0x000a, 0x000b)                   │
│ Behavior     │ worker_behavior_v2                   │
├──────────────┴─────────────────────────────────────┤
│ State Blob (WorkerActor-specific):                 │
│   requests_pending: 3                              │
│   avg_latency_us: 142                              │
│   batch_size: 16                                   │
└────────────────────────────────────────────────────┘
```

---

## 9. Command Input/Output Contracts

### 9.1 `/actor <id> show`

| Direction | Type | Description |
|-----------|------|-------------|
| Request | `InspectStateRequest` | `include_state=true`, `include_mailbox=true`, `include_children=true` |
| Reply | `InspectStateReply` | Full metadata + state_blob + mailbox snapshot + child list |
| Output | Key-value table | Rendered by formatter |

### 9.2 `/actor <id> kill`

| Direction | Type | Description |
|-----------|------|-------------|
| Request | `KillRequest` | `target_actor_id`, `force=false` |
| Reply | `KillReply` | `success`, `error_code` (if any) |
| Output | Status line | `Actor 5 killed.` or `Error: unable to kill system actor.` |

### 9.3 `/actor list`

| Direction | Type | Description |
|-----------|------|-------------|
| Request | `ListActorsRequest` | `cursor`, `limit=50`, `filter` (optional) |
| Reply | `ListActorsReply` | `actors[]`, `next_cursor` |
| Output | Paged table | Interactive paging with filter/search |

### 9.4 `/system stats`

| Direction | Type | Description |
|-----------|------|-------------|
| Request | `SystemStatsRequest` | (empty) |
| Reply | `SystemStatsReply` | `total_actors`, `running`, `idle`, `mailbox_total_depth`, `worker_count`, `memory_active_bytes`, `scheduler_utilization` |
| Output | Key-value table + sparkline | Aggregated system summary |

### 9.5 `/system memory`

| Direction | Type | Description |
|-----------|------|-------------|
| Request | `MemoryStatsRequest` | `actor_id` (optional, null = system-wide) |
| Reply | `MemoryStatsReply` | `active_bytes`, `peak_bytes`, `segment_count`, `slab_hit_rate`, `fragmentation_pct` |
| Output | Key-value table | Memory subsystem snapshot |

### 9.6 `/metrics show`

| Direction | Type | Description |
|-----------|------|-------------|
| Request | `MetricsRequest` (existing) | (empty) |
| Reply | `MetricsResponse` (existing) | OpenMetrics text |
| Output | Raw text | Forwarded OpenMetrics output |

---

## 10. Design Rationale

### 10.1 Why a Trie Instead of a Flat Switch?

- **Extensibility**: New commands are added by inserting a node in the tree — no central dispatch to modify.
- **Help at every level**: `/actor` shows all actor subcommands; `/actor <id>` shows actions on that actor.
- **Tab-completion is natural**: The tree structure maps directly to prefix-based auto-complete.
- **Validation is structural**: Unknown commands are caught at tree traversal time with a clear error path ("Unknown command 'delete'. Did you mean 'kill'?").

### 10.2 Why Messages Instead of Direct Memory Reads?

- **Thread safety**: Actors run on arbitrary worker threads. Reading actor state directly from the CLI thread requires locking or risks torn reads.
- **Consistent snapshot**: The target actor handles the inspect request at a quiescent point — between message handlers — producing a self-consistent snapshot.
- **Extensibility**: New introspection capabilities are added via new request types, not new memory access patterns.
- **Remote CLI**: The message-based approach works identically whether the CLI is local (in-process) or remote (attached via socket).

### 10.3 Why a Dedicated I/O Thread?

- `std::getline(stdin)` is a blocking syscall. If the CLI Actor is scheduled on a shared worker thread, it blocks that worker for potentially minutes while waiting for input.
- A dedicated thread isolates blocking I/O from compute. The CLI thread does nothing but read input and enqueue tokens — zero CPU overhead on compute workers.

### 10.4 Why `to_metadata()` on the Base Class?

- The CLI should not need to know about every actor type. `to_metadata()` provides a common interface that every actor implements.
- Actor-type-specific details go into `serialize_state()`, which the CLI renders as an opaque blob or pretty-prints via a registered formatter for that type.
- This allows third-party actor types to expose inspectable state without modifying the CLI subsystem.

---

## 11. Configuration

### TOML Config

```toml
[system]
scheduler_threads = 4

[system.cli]
enabled = true
listen_path = "/tmp/hpactor/system.sock"
tcp_port = 0
default_format = "pretty"
page_size = 50
```

### C++ Config

```cpp
struct CliConfig {
    bool enabled = true;
    std::string listen_path;       // UDS path, empty = stdin/stdout
    uint16_t tcp_port = 0;        // TCP port, 0 = disabled
    std::string default_format = "pretty";
    uint32_t page_size = 50;
};
```

If `enabled = false`, no CLI Actor is spawned and no listening sockets are created. The overhead is zero.

---

## 12. Open Questions

1. **Command authorization**: Should certain commands (kill, restart) require authentication? A simple shared-secret token in the attach handshake would suffice for initial implementation.
2. **Streaming commands**: `/metrics watch` and `/monitor start` imply long-lived streaming responses. Should these use a separate connection or an in-band streaming protocol (SSE-style)?
3. **History and line editing**: Should the CLI bundle GNU readline or libedit for history, or rely on the invoking shell's readline when attached locally?
4. **Multi-system attach**: Should a single CLI instance attach to multiple ActorSystems simultaneously? The `system` selector would be needed at the prompt level.
5. **State blob formatter registry**: Should actor types be able to register a custom pretty-printer for their `serialize_state()` blob, or is raw hex/protobuf-debug-string sufficient?
