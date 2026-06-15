# CLI Architecture

High-level architecture for the HPActor CLI subsystem — interactive command
dispatch, command registration, output formatting, transport-agnostic sessions,
and remote CLI client/server protocol.

## Documents

| Document | Description |
|----------|-------------|
| [2026-06-15-cli-architecture-design.md](2026-06-15-cli-architecture-design.md) | CLI architecture standardization: layered `ICliCommandHost`/`ISystemCliHost`/`ILifecycleCliHost` host interfaces, `CliClientActor` remote CLI client, protobuf `CliCommand`/`CliResponse` wire protocol with dual binary + HTTP JSON transport, `CliServerActor` multi-listener changes, three-port backward compatibility layout, and command handler migration path. |

## Key Components

| Component | Header | Purpose |
|-----------|--------|---------|
| `CliActor` | `cli_actor.hpp` | Stdin-based interactive CLI daemon actor |
| `CliServerActor` | `cli_server_actor.hpp` | Socket-based CLI server (UDS + TCP) |
| `CliClientActor` | `cli_client_actor.hpp` | Remote CLI client actor (protobuf/HTTP JSON transport) |
| `CliSession` | `cli_session.hpp` | Transport-agnostic command processor |
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

## Architecture Diagram

```
┌──────────────────────────────────────────────────┐
│ ICommand (self-registering, standardized)         │
└────────────────────┬─────────────────────────────┘
                     │ uses
┌────────────────────▼─────────────────────────────┐
│ CommandContext                                    │
│ + ICliCommandHost*   command_host                 │
│ + ISystemCliHost*    system_host                  │
│ + ILifecycleCliHost* lifecycle_host               │
│ + OutputFormatter*   output                       │
└────────────────────┬─────────────────────────────┘
                     │ depends on
┌────────────────────▼─────────────────────────────┐
│ Host Interfaces (abstract)                        │
│ ICliCommandHost   — inspect, kill, enumerate...   │
│ ISystemCliHost    — stats, memory, dlq...         │
│ ILifecycleCliHost — drain, shutdown               │
└──┬──────────────────┬──────────────────┬─────────┘
   │                  │                  │
┌──▼──────┐  ┌────────▼──────┐  ┌───────▼──────────┐
│CliActor │  │CliServerActor │  │CliClientActor     │
│(stdin)  │  │(socket server)│  │(remote client)    │
└─────────┘  └───────────────┘  └───────────────────┘
```
