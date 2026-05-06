# Actor System CLI Interactive Design Specification

**Date:** 2026-05-05
**Status:** Draft
**Author:** HPActor Team

---

## Overview

Add an interactive CLI subsystem for introspecting and managing a running HPActor system. The CLI uses a hierarchical command tree for extensible multi-level command dispatching and an asynchronous request-response "Inspect Message" pattern for thread-safe actor state inspection. A dedicated I/O thread isolates blocking stdin/socket reads from compute workers.

## Goals

- **Multi-level command hierarchy**: Trie-based command registry with contextual help and tab-completion at every level
- **Thread-safe actor inspection**: `InspectStateRequest`/`InspectStateReply` message pair — the CLI never reads actor memory directly
- **Dedicated I/O thread**: Blocks on stdin or UDS socket without stalling compute workers
- **Paged list output**: Cursor-based actor enumeration from the sharded Registry, 50 actors per page
- **Remote attach**: UDS/TCP socket for connecting to a running system from a separate process
- **Multiple output formats**: Pretty (ANSI box-drawing), JSON, YAML, tabular (grep-friendly)
- **Virtual `to_metadata()` interface**: Every actor exposes a lightweight, publicly-inspectable summary; actor-type-specific state goes in `serialize_state()`
- **No exceptions, no RTTI**: Consistent with HPActor design constraints
- **No external dependencies**: Lexer, formatters, command tree are all header-only C++20

## Non-Goals

- Scripting language (Lua/Python embedding) for CLI automation — stdin pipe with `--format json` covers scripting use cases
- Web-based dashboard (Grafana is the separate metrics path)
- gRPC management API — this is an interactive debugging tool, not a management plane
- Command history/readline integration — relies on invoking shell's readline for local attach; remote attach provides its own basic line editing
- Role-based access control for commands — single shared-secret token for remote attach, no per-command authorization
- Multi-system attach (one CLI session connected to multiple ActorSystems) — future work

---

## Architecture

### Component Diagram

```
ActorSystem
├── CliActor (BlockingActor, system actor, dedicated I/O thread)
│   ├── InputReader (blocks on stdin or UDS socket accept/read)
│   ├── Lexer (whitespace tokenizer, flag/option extraction)
│   ├── CommandTree (trie of CommandNode, built at actor init)
│   ├── RequestDispatcher (sends InspectState, Kill, List, Stats messages)
│   ├── OutputFormatter (Pretty, JSON, YAML, Tabular renderers)
│   └── Pager (interactive paging state machine for list commands)
├── ActorRegistry (sharded, cursor-based enumerate() for /actor list)
├── MetricsActor (existing, handles MetricsRequest for /metrics show)
└── AbstractActor::to_metadata() (virtual, returns ActorMetadata for any actor)
```

### Data Flow — Inspect Command

```
[CLI I/O Thread]                    [Target Actor Thread]
────────────────                    ─────────────────────
stdin: "/actor 5 show"
  │
  ├─ Lexer: [/][actor][5][show]
  ├─ CommandTree traversal:
  │   root → "actor" → <id:5> → "show"
  ├─ ShowCommand::execute(ctx):
  │   ├─ Build InspectStateRequest
  │   │   include_state=true
  │   │   include_mailbox=true
  │   │   include_children=true
  │   └─ send(target=5, request)
  │                                   │
  │                                   ├─ Mailbox receives InspectStateRequest
  │                                   ├─ EventBasedActor::receive()
  │                                   │   dispatches to system handler
  │                                   ├─ calls this->to_metadata()
  │                                   ├─ calls this->serialize_state()
  │                                   ├─ builds InspectStateReply
  │                                   └─ reply(reply)
  │                                   │
  ├─ Receives InspectStateReply       │
  ├─ OutputFormatter::key_value()     │
  ├─ OutputFormatter::raw(state_blob) │
  └─ writes to stdout                 │
```

### Data Flow — List Command (Paged)

```
[CLI I/O Thread]                    [ActorRegistry]
────────────────                    ──────────────
stdin: "/actor list"
  │
  ├─ ListCommand::execute(ctx):
  │   ├─ Cursor c{0, 0}
  │   ├─ PageResult page = registry.enumerate(c, 50)
  │   ├─ OutputFormatter::table(columns, rows)
  │   └─ writes page to stdout
  │
stdin: <Enter>  (next page)
  │
  ├─ Pager: has next_cursor? yes
  ├─ PageResult page = registry.enumerate(next_cursor, 50)
  ├─ OutputFormatter::table(columns, rows)
  └─ writes page to stdout
```

### Directory Layout

```
include/hpactor/cli/
    cli_config.hpp              // CliConfig struct
    cli_actor.hpp               // CliActor (BlockingActor subclass)
    command_node.hpp             // CommandNode trie data structure
    command_context.hpp          // CommandContext (args, params, system ptr)
    lexer.hpp                   // Lexer: string → vector<Token>
    token.hpp                   // Token, TokenType enum
    output_formatter.hpp        // OutputFormatter interface
    pretty_formatter.hpp        // PrettyFormatter (ANSI box-drawing)
    json_formatter.hpp          // JsonFormatter (machine-readable)
    tabular_formatter.hpp       // TabularFormatter (whitespace-aligned)
    pager.hpp                   // Pager state machine
    commands/
        show_command.hpp        // /actor <id> show
        kill_command.hpp        // /actor <id> kill
        dump_command.hpp        // /actor <id> dump
        trace_command.hpp       // /actor <id> trace
        link_command.hpp        // /actor <id> link
        mailbox_command.hpp     // /actor <id> mailbox
        list_command.hpp        // /actor list
        system_stats_command.hpp  // /system stats
        system_list_command.hpp   // /system list
        system_config_command.hpp // /system config
        system_threads_command.hpp // /system threads
        system_memory_command.hpp  // /system memory
        metrics_show_command.hpp   // /metrics show
        metrics_watch_command.hpp  // /metrics watch
        topology_show_command.hpp  // /topology show
        topology_graph_command.hpp // /topology graph
        topology_restart_command.hpp // /topology restart
        monitor_start_command.hpp  // /monitor start
        monitor_stop_command.hpp   // /monitor stop
        monitor_log_command.hpp    // /monitor log

src/cli/
    cli_actor.cpp               // CliActor implementation
    command_node.cpp             // CommandNode tree building
    lexer.cpp                    // Lexer implementation
    pretty_formatter.cpp        // PrettyFormatter implementation
    json_formatter.cpp          // JsonFormatter implementation
    tabular_formatter.cpp       // TabularFormatter implementation
    pager.cpp                   // Pager implementation
    commands/
        show_command.cpp
        kill_command.cpp
        dump_command.cpp
        list_command.cpp
        system_stats_command.cpp
        system_list_command.cpp
        system_memory_command.cpp
        metrics_show_command.cpp
        topology_show_command.cpp
        monitor_start_command.cpp

protos/hpactor/
    cli_messages.proto           // InspectState, Kill, List, Stats, Memory messages

tests/cli/
    test_lexer.cpp               // Tokenization correctness
    test_command_node.cpp        // Tree traversal, help text
    test_pretty_formatter.cpp    // Output formatting
    test_command_execution.cpp   // Inspect, List, Kill flows
    test_cli_integration.cpp     // Full CLI actor spawn + command dispatch
```

---

## Component Specification

### 1. `cli_config.hpp` — Configuration

```cpp
namespace hpactor::cli {

struct CliConfig {
    bool enabled = true;
    std::string listen_path;       // UDS path; empty = stdin/stdout only
    uint16_t tcp_port = 0;        // TCP management port; 0 = disabled
    std::string default_format = "pretty";  // pretty, json, yaml, tabular
    uint32_t page_size = 50;      // actors per page for list commands
    std::string auth_token;       // shared secret for remote attach; empty = no auth
};

}  // namespace hpactor::cli
```

TOML binding:

```toml
[system.cli]
enabled = true
listen_path = "/tmp/hpactor/system.sock"
tcp_port = 0
default_format = "pretty"
page_size = 50
# auth_token = ""  # unset = no authentication
```

### 2. `token.hpp` / `lexer.hpp` — Input Tokenization

```cpp
enum class TokenType {
    Slash,          // /
    Keyword,        // actor, show, list, etc.
    Parameter,      // 0x123, echo-actor-1, etc.
    Flag,           // --detail, --no-pager
    FlagWithArg,    // --format json, --filter Worker
    Eof,
};

struct Token {
    TokenType type;
    std::string value;
    std::optional<std::string> arg;  // populated for FlagWithArg
};

class Lexer {
public:
    explicit Lexer(const std::string& input);

    // Returns the next token, or Token{TokenType::Eof} at end.
    Token next();

    // Peek without consuming.
    Token peek() const;

    // Remaining unlexed input (for error messages).
    std::string remainder() const;

private:
    std::string input_;
    size_t pos_ = 0;
};
```

**Lexer Rules:**
1. Whitespace is the token separator.
2. A leading `/` produces `TokenType::Slash` (optional — auto-inserted if missing).
3. Tokens starting with `--` are flags. If the next token does not start with `--`, the flag consumes it as an argument (`TokenType::FlagWithArg`).
4. All other tokens are `TokenType::Keyword`. The parse step determines if a keyword is actually a parameter (e.g., `0x123` under an `<id>` node).

**Edge cases:**
- Empty input → single `Eof` token.
- Quoted strings: `"actor name with spaces"` → one `Parameter` token with the quoted content (value includes spaces, quotes stripped).
- Escape sequences: `\n`, `\t`, `\\` in quoted strings.
- Leading/trailing whitespace → ignored.

### 3. `command_node.hpp` — Command Tree

```cpp
struct CommandNode {
    std::string keyword;
    std::string help_text;
    bool is_parameter = false;  // true for <id>, <filter>, etc.

    // Only one of these is set:
    std::optional<std::function<result<void>(CommandContext&)>> execute;
    std::vector<std::unique_ptr<CommandNode>> children;

    // Builder API
    CommandNode* add_child(std::string kw, std::string help,
                           bool is_param = false);

    // Register leaf action
    void on_execute(std::function<result<void>(CommandContext&)> fn);

    // Lookup child by keyword. If is_parameter, matches any keyword
    // that is not itself a child keyword.
    CommandNode* find_child(const std::string& token,
                            std::string& param_value) const;

    // Generate help text recursively for this node and all children.
    std::string help(int indent = 0) const;
};
```

**Tree traversal algorithm:**

```
function execute(root, tokens, ctx):
    node = root
    for token in tokens:
        child = node.find_child(token, param_value)
        if child is null:
            return error("Unknown command '{token}'. Did you mean: {suggestions}")
        if child.is_parameter:
            ctx.params[child.keyword] = param_value
        node = child
    if node.execute is null:
        return error("Incomplete command. Available subcommands: {node.children}")
    return (*node.execute)(ctx)
```

**Help generation:**

```
hpactor> /actor
Available commands under /actor:
  <id>         Actor ID (hex, decimal, or registered name)
  list         List all actors in the system

hpactor> /actor 5
Available commands for actor 5:
  show         Display actor metadata, state, mailbox, and children
  kill         Terminate the actor (graceful shutdown)
  dump         Full state dump including raw memory stats
  trace        Enable per-message tracing for this actor
  link         Show linked and monitored actors
  mailbox      Show mailbox statistics

hpactor> /actor 5 show --help
Usage: /actor <id> show [--detail] [--format <fmt>]
  --detail       Include full state blob and child details
  --format <fmt> Output format: pretty (default), json, yaml, tabular
```

**Fuzzy matching:** When `find_child()` returns null, suggest the closest matching command using Levenshtein distance ≤ 2. This catches typos: `"shwo"` → "Did you mean 'show'?"

### 4. `command_context.hpp` — Execution Context

```cpp
struct CommandContext {
    std::vector<std::string> args;               // remaining positional args
    std::map<std::string, std::string> params;   // named params and flags
    ActorSystem* system;                         // access to registry, scheduler, config
    CliActor* cli_actor;                         // for sending messages
    OutputFormatter* output;                     // selected formatter
    bool paged = false;                          // interactive paging enabled
    uint32_t page_size = 50;                     // items per page

    // Convenience accessors
    bool has_flag(const std::string& name) const;
    std::optional<std::string> get_param(const std::string& name) const;
    std::string format() const;  // returns "pretty", "json", "yaml", or "tabular"
};
```

### 5. `output_formatter.hpp` — Output Rendering

```cpp
class OutputFormatter {
public:
    virtual ~OutputFormatter() = default;

    // Section header with optional underline.
    virtual void header(const std::string& title) = 0;

    // Column-aligned table.
    virtual void table(const std::vector<std::string>& columns,
                       const std::vector<std::vector<std::string>>& rows) = 0;

    // Key-value pairs, rendered appropriately for the format.
    virtual void key_value(const std::map<std::string, std::string>& pairs) = 0;

    // Hierarchical tree (for topology graph).
    virtual void tree(const TreeNode& root) = 0;

    // Unformatted text passthrough (metrics output, state blobs).
    virtual void raw(const std::string& text) = 0;

    // Error message in appropriate styling.
    virtual void error(const std::string& message) = 0;

    // Finalize and return the complete formatted output as a string.
    virtual std::string finalize() = 0;

    // Factory
    static std::unique_ptr<OutputFormatter> create(const std::string& format);
};
```

**PrettyFormatter** conventions:
- Box-drawing characters: `─`, `│`, `┌`, `┐`, `└`, `┘`, `├`, `┤`, `┬`, `┴`, `┼`
- ANSI color: bold white for headers, cyan for keys, green for values, red for errors
- Auto-sized columns based on content width (capped at terminal width)
- Respects `COLUMNS` environment variable; defaults to 80 columns

**JsonFormatter** conventions:
- One JSON object per command output (no streaming JSON — commands are request-response)
- Keys: snake_case
- Actor IDs as hex strings (`"0x0005"`)
- State blobs as base64-encoded strings

**TabularFormatter** conventions:
- Whitespace-aligned, no ANSI
- Pipe between columns: no (plain whitespace for `awk` compatibility)
- Empty values: `-`

### 6. `pager.hpp` — Interactive Paging

```cpp
class Pager {
public:
    Pager(uint32_t page_size, OutputFormatter* output);

    // Display one page. Returns true if there's more data.
    // `render_page` is called with (offset, limit) to render one page.
    bool show_page(uint32_t total_items,
                   std::function<void(uint32_t offset, uint32_t limit)> render_page);

    // Handle user input after a page is displayed.
    enum class Action { Next, Previous, Quit, Search, First, Last };
    Action handle_input(const std::string& input);

private:
    uint32_t page_size_;
    uint32_t current_offset_ = 0;
    std::string last_search_term_;
    OutputFormatter* output_;
};
```

**Paging UX:**
```
── Actors (page 1, showing 1-50 of 1,283) ────────────────────────────
 ID      Type              State        Uptime     Mailbox
 0x0001  SystemGuardian    Running      12m 03s     0
 ...
── n(ext), p(rev), q(uit), /<search>, g(goto page) ──────────────────
```

### 7. `cli_actor.hpp` — CLI Actor

```cpp
namespace hpactor::cli {

class CliActor : public BlockingActor {
public:
    explicit CliActor(ActorSystem& system, const CliConfig& config);

    // BlockingActor interface
    void init() override;
    void run_once() override;  // one iteration: read line → parse → execute
    void on_exit() override;

    // Called from commands to send introspection messages
    void send_inspect_request(ActorId target, InspectStateRequest req,
                              std::function<void(InspectStateReply)> callback);

    void send_kill_request(ActorId target, KillRequest req,
                           std::function<void(KillReply)> callback);

    // Accessors
    ActorSystem& system() { return system_; }
    const CliConfig& config() const { return config_; }
    OutputFormatter* formatter() { return formatter_.get(); }

private:
    void build_command_tree();
    void process_command(const std::string& line);
    void print_prompt();
    void print_greeting();

    ActorSystem& system_;
    CliConfig config_;
    std::unique_ptr<CommandNode> command_tree_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
    int input_fd_ = STDIN_FILENO;       // or accepted socket fd
    int output_fd_ = STDOUT_FILENO;     // or accepted socket fd
};

}  // namespace hpactor::cli
```

**Lifecycle:**
1. `init()`: Build the command tree (calls `build_command_tree()`), open listening socket if configured, print greeting.
2. `run_once()` loop: Print prompt → block on `read()` from input_fd → lex → parse → execute command → write output → repeat.
3. `on_exit()`: Close listening socket, drain pending callbacks, print farewell.

### 8. Protobuf Messages — `protos/hpactor/cli_messages.proto`

```protobuf
syntax = "proto3";
package hpactor.cli;

// ── Actor Inspection ──

message InspectStateRequest {
    uint64 target_actor_id = 1;
    repeated string fields = 2;        // empty = all fields
    bool include_state = 3;
    bool include_mailbox = 4;
    bool include_children = 5;
}

message InspectStateReply {
    uint64 actor_id = 1;
    ActorMetadata metadata = 2;
    bytes state_blob = 3;
    MailboxSnapshot mailbox = 4;
    repeated ChildInfo children = 5;
}

message ActorMetadata {
    uint64 actor_id = 1;
    string actor_type = 2;
    string state = 3;
    uint64 incarnation = 4;
    uint64 messages_processed = 5;
    uint64 uptime_ms = 6;
    string behavior_name = 7;
}

message MailboxSnapshot {
    uint32 depth = 1;
    uint64 total_enqueued = 2;
    uint64 total_dequeued = 3;
    uint64 max_depth = 4;
    uint32 high_priority_depth = 5;
}

message ChildInfo {
    uint64 actor_id = 1;
    string actor_type = 2;
    string state = 3;
}

// ── Actor Termination ──

message KillRequest {
    uint64 target_actor_id = 1;
    bool force = 2;                    // true = immediate, false = graceful
}

message KillReply {
    bool success = 1;
    uint32 error_code = 2;             // 0 = success
    string error_message = 3;
}

// ── Actor Listing ──

message ListActorsRequest {
    uint32 shard_index = 1;            // cursor: shard
    uint32 offset = 2;                 // cursor: offset within shard
    uint32 limit = 3;                  // max actors to return (default 50)
    string filter = 4;                 // optional: substring match on type name
}

message ListActorsReply {
    repeated ActorMetadata actors = 1;
    bool has_more = 2;
    uint32 next_shard_index = 3;       // cursor for next page
    uint32 next_offset = 4;
    uint32 total_count = 5;            // approximate total (may be stale)
}

// ── System Stats ──

message SystemStatsRequest {
    // empty
}

message SystemStatsReply {
    uint64 total_actors = 1;
    uint64 running_actors = 2;
    uint64 idle_actors = 3;
    uint64 terminated_actors = 4;
    uint64 total_mailbox_depth = 5;
    uint32 worker_count = 6;
    double scheduler_utilization = 7;  // 0.0–1.0
    uint64 memory_active_bytes = 8;
    uint64 memory_peak_bytes = 9;
    uint64 uptime_ms = 10;
}

// ── System Memory ──

message MemoryStatsRequest {
    optional uint64 actor_id = 1;      // null = system-wide
}

message MemoryStatsReply {
    uint64 active_bytes = 1;
    uint64 peak_bytes = 2;
    uint32 segment_count = 3;
    double slab_hit_rate = 4;          // 0.0–1.0
    double fragmentation_pct = 5;      // 0.0–100.0
}

// ── Topology ──

message TopologyShowRequest {
    // empty
}

message TopologyShowReply {
    string source_file = 1;            // original TOML path
    uint32 actor_count = 2;
    uint32 dispatcher_count = 3;
    TopologyNode root = 4;             // tree representation
}

message TopologyNode {
    string name = 1;
    string actor_type = 2;
    string behavior = 3;
    repeated TopologyNode children = 4;
}

message TopologyRestartRequest {
    string actor_name = 1;             // configured name from TOML
}

message TopologyRestartReply {
    bool success = 1;
    uint64 new_actor_id = 2;
    string error_message = 3;
}
```

---

## 9. `to_metadata()` — Virtual Interface on AbstractActor

```cpp
// In include/hpactor/actor/abstract_actor.hpp

class AbstractActor {
public:
    // Existing interface...

    // Returns a lightweight, publicly-inspectable metadata snapshot.
    // Called from the actor's own thread (safe to read internal state).
    // Base implementation fills common fields; subclasses may override
    // to add type-specific metadata.
    virtual cli::ActorMetadata to_metadata() const;

    // Returns an opaque protobuf-serialized state blob for the actor's
    // internal state. Called only when include_state=true in the request.
    // Default returns empty. Subclasses override to expose internals.
    virtual bytes serialize_state() const;

    // Returns mailbox stats if this actor has a mailbox.
    // Default returns empty snapshot. Blocking/non-mailbox actors return empty.
    virtual cli::MailboxSnapshot mailbox_snapshot() const;
};
```

**Default implementation:**

```cpp
cli::ActorMetadata AbstractActor::to_metadata() const {
    cli::ActorMetadata m;
    m.actor_id = id().value();
    m.actor_type = type_name();
    m.state = actor_state_to_string(state_.load());
    m.incarnation = incarnation_;
    m.messages_processed = messages_processed_.load();
    m.uptime_ms = Clock::now() - spawn_time_;
    m.behavior_name = current_behavior_name_;
    return m;
}
```

---

## 10. Command Registration & Extensibility

### 10.1 Built-in Commands

Built-in commands are registered in `CliActor::build_command_tree()`. Each command is a leaf node that stores a `std::function<result<void>(CommandContext&)>`.

```cpp
void CliActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");

    // /actor <id> ...
    auto* actor = root->add_child("actor", "Actor operations");
    auto* actor_id = actor->add_child("<id>", "Target actor ID", /*is_param=*/true);

    actor_id->add_child("show", "Display actor metadata")
        ->on_execute(&Commands::execute_show);
    actor_id->add_child("kill", "Terminate actor")
        ->on_execute(&Commands::execute_kill);
    actor_id->add_child("dump", "Full diagnostic dump")
        ->on_execute(&Commands::execute_dump);
    actor_id->add_child("trace", "Enable message tracing")
        ->on_execute(&Commands::execute_trace);
    actor_id->add_child("link", "Show link/monitor relationships")
        ->on_execute(&Commands::execute_link);
    actor_id->add_child("mailbox", "Mailbox statistics")
        ->on_execute(&Commands::execute_mailbox);

    // /actor list
    actor->add_child("list", "List all actors")
        ->on_execute(&Commands::execute_list);

    // /system ...
    auto* system_cmd = root->add_child("system", "System operations");
    system_cmd->add_child("list", "List system actors")
        ->on_execute(&Commands::execute_system_list);
    system_cmd->add_child("stats", "System statistics")
        ->on_execute(&Commands::execute_system_stats);
    system_cmd->add_child("config", "Show system config")
        ->on_execute(&Commands::execute_system_config);
    system_cmd->add_child("threads", "Thread pool status")
        ->on_execute(&Commands::execute_system_threads);
    system_cmd->add_child("memory", "Memory subsystem stats")
        ->on_execute(&Commands::execute_system_memory);

    // /metrics ...
    auto* metrics = root->add_child("metrics", "Metrics operations");
    metrics->add_child("show", "Show current metrics snapshot")
        ->on_execute(&Commands::execute_metrics_show);
    metrics->add_child("watch", "Stream metrics (CTRL-C to stop)")
        ->on_execute(&Commands::execute_metrics_watch);

    // /topology ...
    auto* topo = root->add_child("topology", "Topology operations");
    topo->add_child("show", "Show topology tree")
        ->on_execute(&Commands::execute_topology_show);
    topo->add_child("graph", "Show topology as graph")
        ->on_execute(&Commands::execute_topology_graph);
    topo->add_child("restart", "Restart a topology actor")
        ->on_execute(&Commands::execute_topology_restart);

    // /monitor ...
    auto* mon = root->add_child("monitor", "Event monitoring");
    mon->add_child("start", "Start event monitor")
        ->add_child("<filter>", "Event filter expression", /*is_param=*/true)
        ->on_execute(&Commands::execute_monitor_start);
    mon->add_child("stop", "Stop event monitor")
        ->on_execute(&Commands::execute_monitor_stop);
    mon->add_child("log", "Show recent monitored events")
        ->on_execute(&Commands::execute_monitor_log);

    command_tree_ = std::move(root);
}
```

### 10.2 User-Registered Commands (Future)

For extensibility without modifying the CLI subsystem, actor types (or shared libraries) could register additional command nodes:

```cpp
// Hypothetical future extension:
class CliActor {
public:
    // Register a custom command subtree. The callback receives
    // the CommandNode* for the parent so it can add children.
    void register_command(
        const std::string& parent_path,  // e.g., "/actor <id>"
        std::function<void(CommandNode*)> builder
    );
};
```

This is deferred — the initial implementation hardcodes the built-in command tree.

---

## 11. Remote Attach Protocol

### 11.1 Connection Establishment

```
Client                          Server (CliActor)
──────                          ─────────────────
connect(UDS path)  ──────────▶  accept()
                                  │
send(AuthRequest{token}) ─────▶  verify token
                                  │
                     ◀──────────  AuthResponse{ok, greeting}
                                  │
enter REPL loop:                  │
  send(CommandRequest{"/actor 5 show"})
                     ◀──────────  CommandResponse{formatted_output}
  send(CommandRequest{"/actor list"})
                     ◀──────────  CommandResponse{formatted_output}
  ...
  send(QuitRequest{})
                     ◀──────────  Goodbye, close connection
```

### 11.2 Wire Protocol

All frames are newline-terminated JSON for simplicity (human-debuggable with `nc`):

```json
→ {"type":"auth","token":"<shared_secret>"}
← {"type":"auth_ok","greeting":"HPActor CLI v1.0 — 1,283 actors running"}
← {"type":"auth_error","message":"Invalid token"}

→ {"type":"command","line":"/actor 5 show"}
← {"type":"output","format":"pretty","body":"┌─── Actor 5 ───┐\n│ ... │"}

→ {"type":"command","line":"/actor 5 show --format json"}
← {"type":"output","format":"json","body":"{\"actor_id\":5,...}"}

→ {"type":"quit"}
← {"type":"goodbye","message":"Disconnecting..."}
```

For paged output, the server sends multiple `output` messages separated by `prompt` messages:

```
← {"type":"output","body":"── Page 1 of 26 ──\n..."}
← {"type":"prompt","message":"n(ext), p(rev), q(uit):"}
→ {"type":"command","line":"n"}
← {"type":"output","body":"── Page 2 of 26 ──\n..."}
```

### 11.3 `hpactor` CLI Frontend

A thin C++ executable (`tools/hpactor-cli/`) provides the remote-attach frontend:

```bash
# Attach to local system via UDS
hpactor attach --socket /tmp/hpactor/system.sock

# Attach to remote system via TCP
hpactor attach --host 10.0.1.5 --port 9090 --token my-secret

# One-shot command (no REPL)
hpactor attach --socket /tmp/hpactor/system.sock --exec "/actor list --format json"

# Pipe command from stdin
echo "/actor 5 show" | hpactor attach --socket /tmp/hpactor/system.sock
```

The frontend handles connection, authentication, and local line editing (via libedit or raw terminal mode). It is a standalone executable, not part of `hpactor_lib`.

---

## 12. TypeTags

New TypeTag entries for CLI messages:

```cpp
enum class TypeTag : uint32_t {
    // ... existing ...
    // Metrics subsystem (0x40 – 0x4F)
    MetricsRequestTag  = 0x40,   // existing
    MetricsResponseTag = 0x41,   // existing

    // CLI interactive subsystem (0x50 – 0x5F) — NEW
    InspectStateRequestTag    = 0x50,
    InspectStateResponseTag   = 0x51,
    KillRequestTag            = 0x52,
    KillResponseTag           = 0x53,
    ListActorsRequestTag      = 0x54,
    ListActorsResponseTag     = 0x55,
    SystemStatsRequestTag     = 0x56,
    SystemStatsResponseTag    = 0x57,
    MemoryStatsRequestTag     = 0x58,
    MemoryStatsResponseTag    = 0x59,
    TopologyShowRequestTag    = 0x5A,
    TopologyShowResponseTag   = 0x5B,
    TopologyRestartRequestTag = 0x5C,
    TopologyRestartResponseTag = 0x5D,
};
```

These are handled by system dispatch in `EventBasedActor::receive()`, not by user behavior — same pattern as `LinkMsg`/`UnlinkMsg`/`DownMsg`.

---

## 13. Test Plan

### 13.1 Unit Tests

| Test | What It Validates |
|------|-------------------|
| `test_lexer` | Tokenization: keywords, parameters, flags, options, quoted strings, empty input, escape sequences, EOF |
| `test_command_node` | Tree building, `find_child()` exact match + parameter match, fuzzy suggestion, help text generation at each level |
| `test_pretty_formatter` | Box-drawing output, column alignment, ANSI codes, edge cases (narrow terminal, empty values) |
| `test_json_formatter` | Valid JSON output, base64 state blobs, hex actor IDs |
| `test_command_execution` | Each command builds correct request message, handles reply, formats output. Mock target actors return known replies. |
| `test_pager` | Page navigation (next, prev, quit, search), edge cases (empty list, single page, exact boundary) |

### 13.2 Integration Tests

| Test | What It Validates |
|------|-------------------|
| `test_cli_actor_spawn` | CLI Actor initializes, builds command tree, opens socket if configured |
| `test_cli_inspect_flow` | `/actor <id> show` → InspectStateRequest sent → target responds → CLI formats output |
| `test_cli_kill_flow` | `/actor <id> kill` → KillRequest sent → target terminates → CLI prints status |
| `test_cli_list_paging` | `/actor list` with 200 actors → 4 pages, cursor-based enumeration, page navigation |
| `test_cli_remote_attach` | Client connects via UDS → auth → sends command → receives formatted response |
| `test_cli_error_handling` | Unknown command, missing target actor, auth failure, socket disconnect |

### 13.3 Expected Test Count

| Suite | Count |
|-------|-------|
| `test_lexer` | 12 |
| `test_command_node` | 8 |
| `test_pretty_formatter` | 6 |
| `test_json_formatter` | 4 |
| `test_command_execution` | 10 |
| `test_pager` | 5 |
| `test_cli_integration` | 8 |
| **Total** | **53** |

---

## 14. Implementation Phases

### Phase 1: Core CLI Infrastructure (no messages)
- `cli_config.hpp`, `token.hpp`, `lexer.hpp`/`lexer.cpp`
- `command_node.hpp`/`command_node.cpp`
- `command_context.hpp`
- `output_formatter.hpp`, `pretty_formatter.hpp`/`pretty_formatter.cpp`, `json_formatter.cpp`, `tabular_formatter.cpp`
- `pager.hpp`/`pager.cpp`
- Tests: `test_lexer`, `test_command_node`, `test_pretty_formatter`, `test_json_formatter`, `test_pager`

### Phase 2: CLI Actor + Protobuf Messages
- `protos/hpactor/cli_messages.proto` → generated C++
- TypeTag entries (15–28)
- `to_metadata()` / `serialize_state()` / `mailbox_snapshot()` on `AbstractActor`
- `cli_actor.hpp`/`cli_actor.cpp` with command tree, input loop, message dispatch
- System message intercept in `EventBasedActor::receive()` for CLI request tags
- Tests: `test_command_execution`, `test_cli_actor_spawn`

### Phase 3: Command Implementations
- All `commands/*.hpp`/`commands/*.cpp`: show, kill, dump, trace, link, mailbox, list, system_stats, system_list, system_config, system_threads, system_memory, metrics_show, metrics_watch, topology_show, topology_graph, topology_restart, monitor_start, monitor_stop, monitor_log
- Integration with ActorRegistry cursor API, MetricsActor, existing stats

### Phase 4: Remote Attach
- UDS/TCP listener in CliActor
- JSON wire protocol (auth, command, output, prompt, quit message types)
- `tools/hpactor-cli/` frontend executable
- Tests: `test_cli_integration` (full remote attach flow)

### Phase 5: Polish
- ANSI color theming (dark/light terminal detection)
- Command aliases (`/ls` → `/actor list`, `/stat` → `/system stats`)
- `--help` at every node
- Fuzzy matching suggestions for typos
- `hpactor attach --exec` one-shot mode

---

## 15. Open Questions

1. **Command authorization**: Should `/actor <id> kill` require an `--force` confirmation flag? Or is the shared-secret auth token on remote attach sufficient?
2. **Streaming commands**: `/metrics watch` and `/monitor start` produce continuous output. Should these use the same connection with a `stream_id` for multiplexing, or require a separate connection?
3. **History/readline**: Should the local CLI bundle libedit, or just document that `rlwrap hpactor attach ...` provides history?
4. **State blob formatting**: Should actor types register a custom pretty-printer for `serialize_state()`, or fall back to `protobuf::Message::DebugString()` / hex dump?
5. **Command piping**: Should `/actor list --format json | /actor kill` work as a pipe where the output of one command feeds the input of another? Deferred — scripting is done externally via `--exec` and `jq`.
6. **Tab completion contract**: Who drives tab completion — the CLI frontend (by requesting the command tree) or the server (by accepting partial input and returning completions)?
7. **Multi-word parameter parsing**: How to handle an actor name like `"Worker Pool 3"` — quoted string support in the lexer, or forbid spaces in registered actor names?
