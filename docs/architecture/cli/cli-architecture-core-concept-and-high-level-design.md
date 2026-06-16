# CLI Architecture

High-level architecture for the HPActor CLI subsystem — interactive command
dispatch, command registration, output formatting, transport-agnostic sessions,
and multi-role CLI actors for local, remote protobuf, and HTTP JSON clients.

## Multi-Role CLI Actor Design

```
┌──────────────────────────────────────────────────────────────┐
│                    ICommand (self-registering)                │
│                    + path(), help_text(), execute(ctx)        │
└─────────────────────────────┬────────────────────────────────┘
                              │ uses
┌─────────────────────────────▼────────────────────────────────┐
│ CommandContext                                                │
│ + ICliCommandHost*   command_host     (actor operations)      │
│ + ISystemCliHost*    system_host      (system queries)       │
│ + ILifecycleCliHost* lifecycle_host   (drain, shutdown)      │
│ + OutputFormatter*   output           (rendering)            │
│ + ActorSystem*       system           (nullable on remote)   │
└─────────────────────────────┬────────────────────────────────┘
                              │ depends on
┌─────────────────────────────▼────────────────────────────────┐
│ Host Interfaces (abstract)                                    │
│ ICliCommandHost   — inspect, kill, enumerate, quarantine      │
│ ISystemCliHost    — render stats, memory, faults, DLQ         │
│ ILifecycleCliHost — drain, shutdown                           │
└──┬────────────────────┬──────────────────┬───────────────────┘
   │                    │                  │
   │  Role 1: Local     │  Role 2: Remote  │  Role 3: HTTP
   │                    │  Protobuf Client │  JSON Client
   │                    │                  │
┌──▼──────────┐  ┌──────▼──────────┐  ┌───▼──────────────────┐
│CliLocalActor│  │CliClientActor   │  │  Any HTTP client      │
│(stdin)      │  │(stdin, TCP/UDS) │  │  (curl, SDKs, etc.)  │
│             │  │                 │  │                       │
│Raw text     │  │HPAC WireFrame   │  │  POST /cli JSON       │
│payload via  │  │protobuf via     │  │  via HTTPGateway      │
│deliver_local│  │Connection::send │  │                       │
└──────┬──────┘  └──────┬──────────┘  └───┬──────────────────┘
       │                │                 │
       │  same process  │  TCP/UDS        │  HTTP
       ▼                ▼                 ▼
┌──────────────────────────────────────────────────────────────┐
│                     CliSession (shared)                       │
│         Transport-agnostic command dispatch engine            │
│         Tokenize → Walk CommandNode trie → Execute            │
└──────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌──────────────────────────────────────────────────────────────┐
│                        Server Side                            │
│                                                               │
│  ┌──────────────────┐  ┌────────────────┐  ┌───────────────┐ │
│  │ CliServerActor   │  │CliProtoServer  │  │CliHttpServer  │ │
│  │ (legacy raw text)│  │Actor (NEW)     │  │Actor (NEW)    │ │
│  │                  │  │                │  │               │ │
│  │ UDS/TCP acceptor │  │TcpTransport::  │  │HTTPGateway::  │ │
│  │ raw ::read()     │  │listen()        │  │route()        │ │
│  │ \n-delimited     │  │                │  │               │ │
│  │ \0-terminated    │  │WireFrameConn   │  │POST /cli      │ │
│  │                  │  │auto decode     │  │JSON ↔ proto   │ │
│  │                  │  │HPAC frames     │  │               │ │
│  │                  │  │                │  │               │ │
│  │ Deprecated       │  │frame_handler   │  │Any HTTP       │ │
│  │ (kept for compat)│  │→ CliSession    │  │client         │ │
│  └──────────────────┘  └────────────────┘  └───────────────┘ │
│                           All three share CliSession          │
└──────────────────────────────────────────────────────────────┘
```

## Key Components

| Component | Header | Purpose |
|-----------|--------|---------|
| `CliLocalActor` | `cli_local_actor.hpp` | Stdin-based local CLI (raw text payload, `deliver_local`) |
| `CliClientActor` | `cli_client_actor.hpp` | Remote CLI client (HPAC WireFrame + protobuf via `Connection::send()`) |
| `CliServerActor` | `cli_server_actor.hpp` | Legacy raw-text socket server (UDS + TCP, deprecated) |
| `CliProtoServerActor` | `cli_proto_server_actor.hpp` | Protobuf CLI server — `TcpTransport::listen()`, `WireFrameConnection` auto-decode |
| `CliHttpServerActor` | `cli_http_server_actor.hpp` | HTTP JSON CLI server — `HTTPGateway::route(POST /cli)`, JSON ↔ protobuf |
| `CliSession` | `cli_session.hpp` | Transport-agnostic command processor (shared by all actors) |
| `InteractiveCliActor` | `interactive_cli_actor.hpp` | Template Method base for stdin-based CLI actors |
| `CliConnector` | `cli_connector.hpp` | Wrapper around `TcpTransport` for client-side connect |
| `CommandNode` | `command_node.hpp` | Trie-based command tree with parameter matching |
| `ICommand` | `command_registry.hpp` | Self-registering command interface |
| `CommandContext` | `command_context.hpp` | Execution context with host interface pointers |
| `ICliCommandHost` | `cli_command_host.hpp` | Abstract interface for actor operations |
| `ISystemCliHost` | `cli_command_host.hpp` | Abstract interface for system queries |
| `ILifecycleCliHost` | `cli_command_host.hpp` | Abstract interface for lifecycle operations |
| `OutputFormatter` | `output_formatter.hpp` | Pluggable formatters (pretty, json, tabular) |
| `LineEditor` | `line_editor.hpp` | Readline-style input with history and completion |
| `Pager` | `pager.hpp` | Interactive paging for multi-page output |
| `Lexer` | `lexer.hpp` | Tokenizer for CLI input lines |

## Three Role Modes

### Role 1: Local CLI (`CliLocalActor`)

- **Input:** stdin via `LineEditor`
- **Transport:** ActorSystem `deliver_local()` — raw text payloads via `TypedMessage`
- **Command host:** Queries local `ActorSystem` directly (actor inspect, kill, enumerate)
- **Wire format:** Raw text (no framing — messages stay within the actor system)
- **Output:** `printf()` via `OutputFormatter`

### Role 2: Remote Protobuf Client (`CliClientActor`)

- **Input:** stdin via `LineEditor`
- **Transport:** `TcpTransport::connect()` → `Connection::send()` — HPAC WireFrame-encoded protobuf
- **Wire format:** HPAC Frame (magic "HPAC" + big-endian length + protobuf payload)
- **Command host:** Structured RPC (`rpc_method` + `rpc_request`) or command-tree (`path` + `args`)
- **Receive:** `WireFrameConnection::handle_read()` → frame handler → CliResponse decode
- **Server side:** `CliProtoServerActor` — `TcpTransport::listen()` → `WireFrameConnection` auto-decode → `CliSession`
- **Output:** `printf()` via `OutputFormatter`

### Role 3: HTTP JSON Client (any HTTP client)

- **Input:** `POST /cli` with JSON body (`{"path": "system/stats", "params": {...}}`)
- **Transport:** HTTP/1.1 via `HTTPGateway`
- **Wire format:** JSON (standard protobuf JSON mapping)
- **Server side:** `CliHttpServerActor` — `HTTPGateway::route(POST, "/cli", handler)` → `CliSession`
- **Response:** 200 OK with JSON body (`{"content_type": "text/plain", "payload": "..."}`)
- **Clients:** `curl`, Python `requests`, any HTTP library — no HPActor dependency needed

## Connection Matrix

| Client | Server | Transport | Wire Format |
|--------|--------|-----------|-------------|
| `CliLocalActor` (stdin) | (same process) | `deliver_local()` | Raw text via `TypedMessage` |
| `CliClientActor` (TCP) | `CliProtoServerActor` | `TcpTransport` / `WireFrameConnection` | HPAC Frame + protobuf |
| `CliClientActor` (UDS) | `CliProtoServerActor` | `TcpTransport` / `WireFrameConnection` | HPAC Frame + protobuf |
| `curl` / HTTP client | `CliHttpServerActor` | `HTTPGateway` | JSON |
| Legacy `CliClientActor` | `CliServerActor` (legacy) | Raw `::read()`/`::write()` | `\n` / `\0` text |
