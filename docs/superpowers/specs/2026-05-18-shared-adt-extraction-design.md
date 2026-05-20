# Shared ADT Extraction Design

**Date:** 2026-05-18
**Status:** Approved
**Scope:** Extract common user-defined data structures across HPActor into shared Abstract Data Types using C++20 templates.

## Motivation

A codebase-wide audit identified five patterns of duplicated or similar user-defined data structures. Extracting them into shared generic types (templates parameterized by tag type/value type) eliminates code duplication, enforces consistent interfaces, and establishes ADT conventions for the codebase.

## Design Constraints

- C++20 templates, no virtual dispatch (no RTTI, no exceptions)
- Header-only ADT definitions in `include/hpactor/adt/`
- Compile-time type safety via tag types
- Rename everywhere — no backward-compatible aliases
- Zero runtime overhead (same as hand-written equivalents)

---

## Section 1: `Id<Tag, T>` — Opaque Identifier Template

### Current State

Four hand-written classes wrapping `uint64_t` with near-identical interfaces:

| Type | Accessor | Equality | Hash | Factory | Location |
|------|----------|----------|------|---------|----------|
| `ActorId` | `value()` | `==`, `!=` | `std::hash` | constexpr | `types/types.hpp` |
| `MessageId` | `value()` | `==`, `!=` | `std::hash` | static `generate()` | `types/types.hpp` |
| `AlarmHandle` | `id()` | `==`, `!=` | — | explicit ctor | `types/types.hpp` |
| `TimerHandle` | `valid()` | `==`, `!=` | — | explicit ctor | `sched/scheduler.hpp` |

### New ADT

```cpp
// include/hpactor/adt/id.hpp
template <typename Tag, typename T = uint64_t>
class Id {
    T value_{};
public:
    constexpr Id() = default;
    explicit constexpr Id(T v) : value_{v} {}

    [[nodiscard]] constexpr T value() const { return value_; }
    [[nodiscard]] constexpr bool valid() const { return value_ != T{}; }

    friend constexpr bool operator==(Id, Id) = default;
    friend constexpr bool operator!=(Id, Id) = default;
    friend constexpr auto operator<=>(Id, Id) = default;
};

// std::hash specialization
template <typename Tag, typename T>
struct std::hash<Id<Tag, T>> {
    size_t operator()(Id<Tag, T> id) const noexcept {
        return std::hash<T>{}(id.value());
    }
};
```

### Tag Types

```cpp
// include/hpactor/adt/tags.hpp
namespace hpactor {
struct ActorTag {};
struct MessageTag {};
struct AlarmTag {};
struct TimerTag {};
}
```

### Concrete Instantiations

Old hand-written classes are replaced by aliases pointing to the generic template:

```cpp
// include/hpactor/types/types.hpp
using ActorId     = Id<ActorTag>;
using MessageId   = Id<MessageTag>;
using AlarmHandle = Id<AlarmTag>;

// include/hpactor/sched/scheduler.hpp
using TimerHandle = Id<TimerTag>;
```

All existing call sites continue to compile without changes — `ActorId{42}`, `msg_id.value()`, `std::hash<ActorId>{}` all work identically.

### Migration Notes

- `MessageId::generate()` becomes a free function `generate_message_id()` returning `MessageId`; call sites that used `MessageId::generate()` update to `generate_message_id()`
- Old class definitions deleted; aliases provide the same names
- `Id<Tag, T>` provides `value()`, `valid()`, `==`, `!=`, `<=>` — all operations the old classes exposed
- `AlarmHandle::id()` maps to `AlarmHandle::value()` via the alias
- `TimerHandle::valid()` maps to `TimerHandle::valid()` via the alias (same name)

### Files Changed

- **New:** `include/hpactor/adt/id.hpp` (~40 lines)
- **New:** `include/hpactor/adt/tags.hpp` (~15 lines)
- **Modified:** `include/hpactor/types/types.hpp` — remove `ActorId`, `MessageId`, `AlarmHandle` class definitions; add `using` aliases
- **Modified:** `include/hpactor/sched/scheduler.hpp` — remove `TimerHandle` class definition; add `using` alias
- **Modified:** ~5 files that call `MessageId::generate()` → update to `generate_message_id()`

---

## Section 2: Result/Status Types

### Current State

`result<T>` already exists as the canonical simple status-or-value type. Three other types follow the same `(status, data)` pattern but carry richer domain metadata:

| Type | Status | Extra Metadata |
|------|--------|---------------|
| `result<T>` | `error` | — |
| `EnqueueResult` | `EnqueueResultCode` | depth, capacity, pressure, target, retry, type_tag, msg_id |
| `TraceParseResult` | `TraceParseStatus` | — (only `TraceContext` data) |
| `SpawnResponse` | `uint32_t error_code` | — (only `ActorAddress` data) |

### Decision

**No new ADT.** `result<T>` already fills the simple case. The three hand-rolled types differ for legitimate domain reasons (mailbox pressure metadata, spawn routing info). Forcing them into a single template would make each call site harder to read.

### Actions

- Add consistency check: ensure `EnqueueResult` and `TraceParseResult` both provide `.status()`, `.ok()`, and data accessors with the same naming convention
- `result<T>` remains in `types/types.hpp` as the canonical simple result type
- No new files

### Files Changed

- **Modified:** `include/hpactor/mailbox/mailbox_policy.hpp` — add `status()`, `ok()` to `EnqueueResult`
- **Modified:** `include/hpactor/tracing/trace_context_parser.hpp` — add `status()`, `ok()` to `TraceParseResult`

---

## Section 3: `NodeIdentity` — Composed Node Descriptor

### Current State

Four structs describing a remote node with substantial field overlap:

```
Member:           endpoint, host, uds_path, acceptors, actor_types,         status, incarnation, last_seen
NodeEndpoint:     endpoint, host, uds_path, acceptors,          tcp_port, is_static_route,       last_seen
PiggybackEntry:   endpoint,                            actor_types, load, acceptors
StaticRouteConfig: endpoint,          address, port
```

Common core `{endpoint, host, uds_path, acceptors}` appears in `Member` and `NodeEndpoint`. `PiggybackEntry` shares `endpoint + acceptors + actor_types`.

### New ADT

```cpp
// include/hpactor/adt/node_identity.hpp
namespace hpactor {

struct NodeIdentity {
    EndPoint endpoint;
    std::string host;
    std::string uds_path;
    std::vector<AcceptorInfo> acceptors;

    bool operator==(const NodeIdentity&) const = default;
};

} // namespace hpactor
```

### After Extraction

```cpp
// net/service_discovery.hpp
struct Member {
    NodeIdentity identity;
    std::vector<ActorType> actor_types;
    MemberStatus status;
    uint64_t incarnation;
    std::chrono::steady_clock::time_point last_seen;
};

// net/registrar.hpp
struct NodeEndpoint {
    NodeIdentity identity;
    uint16_t tcp_port;
    bool is_static_route;
    std::chrono::steady_clock::time_point last_seen;
};

// net/gossip_membership.hpp
struct PiggybackEntry {
    PiggybackType type;
    NodeIdentity identity;
    uint64_t incarnation;
    std::vector<ActorType> actor_types;
    uint32_t load;
};

// StaticRouteConfig — unchanged, only shares endpoint
```

### Migration Notes

- Call sites update from `m.endpoint` → `m.identity.endpoint`, `n.host` → `n.identity.host`, etc.
- `PiggybackEntry` previously had `acceptors` and `actor_types` as separate fields alongside `endpoint`; now `actor_types` stays on the entry while `acceptors` moves into `identity`
- ~30-40 call sites across gossip, registrar, and service discovery code

### Files Changed

- **New:** `include/hpactor/adt/node_identity.hpp` (~20 lines)
- **Modified:** `include/hpactor/net/service_discovery.hpp` — `Member` embeds `NodeIdentity`
- **Modified:** `include/hpactor/net/registrar.hpp` — `NodeEndpoint` embeds `NodeIdentity`
- **Modified:** `include/hpactor/net/gossip_membership.hpp` — `PiggybackEntry` embeds `NodeIdentity`
- **Modified:** `src/net/` — gossip, registrar, service discovery call sites

---

## Section 4: Config Schema — X-Macro Canonical Definition

### Current State

Three representations of the same system configuration, manually kept in sync:

```
SystemDef (TOML)  ──parse──▶  Config (runtime)  ──serialize──▶  BinarySystemDef (mmap)
   ~25 string fields              ~25 typed fields                 ~35 flat POD fields
```

Same pattern for actors, dispatchers, and mailbox defaults:
```
ActorDef (TOML) ──▶ (inline in Config) ──▶ BinaryActorDef
DispatcherDef (TOML) ──▶ (inline in Config) ──▶ BinaryDispatcherDef
MailboxDefaults ──▶ SystemMailboxDef
```

### Constraint

The binary format must remain flat POD with offset-based string references for zero-copy mmap. The TOML format must use `std::string` for flexible parsing. The runtime config needs fast direct field access. These are three different physical layouts for valid reasons.

### Strategy

X-macro table as the single canonical schema. Each field is defined once. The three struct definitions and their conversion functions are generated by including the table with different macro definitions per context.

### X-Macro Tables

```cpp
// include/hpactor/config/system_fields.def
// HPACTOR_SYSTEM_FIELD(cpp_name, cpp_type, toml_key, default_value)

HPACTOR_SYSTEM_FIELD(scheduler_threads,  uint16_t,  "scheduler.threads",          4)
HPACTOR_SYSTEM_FIELD(enable_network,     bool,      "network.enabled",            false)
HPACTOR_SYSTEM_FIELD(listen_port,        uint16_t,  "network.listen_port",        0)
HPACTOR_SYSTEM_FIELD(enable_http,        bool,      "http.enabled",               false)
HPACTOR_SYSTEM_FIELD(http_port,          uint16_t,  "http.port",                  8080)
HPACTOR_SYSTEM_FIELD(enable_coroutines,  bool,      "coroutines.enabled",         false)
HPACTOR_SYSTEM_FIELD(enable_metrics,     bool,      "metrics.enabled",            true)
HPACTOR_SYSTEM_FIELD(enable_cli,         bool,      "cli.enabled",                false)
HPACTOR_SYSTEM_FIELD(cli_listen_path,    string,    "cli.listen_path",            "")
HPACTOR_SYSTEM_FIELD(enable_tracing,     bool,      "tracing.enabled",            true)
HPACTOR_SYSTEM_FIELD(tracing_sample_rate, double,  "tracing.sample_rate",         0.01)
HPACTOR_SYSTEM_FIELD(enable_logging,     bool,      "logging.enabled",            true)
HPACTOR_SYSTEM_FIELD(log_level,          string,    "logging.level",              "info")
HPACTOR_SYSTEM_FIELD(gossip_port,        uint16_t,  "discovery.gossip_port",      0)
HPACTOR_SYSTEM_FIELD(drain_timeout_ms,   uint32_t,  "shutdown.drain_timeout_ms",  5000)
HPACTOR_SYSTEM_FIELD(stop_timeout_ms,    uint32_t,  "shutdown.stop_timeout_ms",   10000)
// ... remaining ~10 fields
```

```cpp
// include/hpactor/config/mailbox_fields.def
// HPACTOR_MAILBOX_FIELD(cpp_name, cpp_type, toml_key, default_value)

HPACTOR_MAILBOX_FIELD(capacity,              uint32_t,          "capacity",               1024)
HPACTOR_MAILBOX_FIELD(byte_capacity,         uint64_t,          "byte_capacity",          65536)
HPACTOR_MAILBOX_FIELD(overflow_policy,       OverflowPolicy,    "overflow_policy",        OverflowPolicy::RejectNewest)
HPACTOR_MAILBOX_FIELD(backpressure_mode,     BackpressureMode,  "backpressure_mode",      BackpressureMode::Disabled)
HPACTOR_MAILBOX_FIELD(soft_watermark_pct,    uint8_t,           "soft_watermark_pct",     70)
HPACTOR_MAILBOX_FIELD(hard_watermark_pct,    uint8_t,           "hard_watermark_pct",     90)
HPACTOR_MAILBOX_FIELD(protected_sys_msgs,    bool,              "protected_sys_msgs",     true)
```

```cpp
// include/hpactor/config/dispatcher_fields.def
// HPACTOR_DISPATCHER_FIELD(cpp_name, cpp_type, default_value)

HPACTOR_DISPATCHER_FIELD(name,          std::string,  "")
HPACTOR_DISPATCHER_FIELD(threads,       uint16_t,     1)
HPACTOR_DISPATCHER_FIELD(cpu_affinity,  std::vector<uint8_t>, {})
```

### Usage Pattern

```cpp
// Runtime Config struct — generated from the table
struct Config {
    #define HPACTOR_SYSTEM_FIELD(name, type, toml, def) type name{def};
    #include "config/system_fields.def"
    #undef HPACTOR_SYSTEM_FIELD
};

// TOML → Config conversion — generated
void Config::load_from_system_def(const SystemDef& def) {
    #define HPACTOR_SYSTEM_FIELD(name, type, toml, def) name = def.name;
    #include "config/system_fields.def"
    #undef HPACTOR_SYSTEM_FIELD
}

// Binary serialization — generated
void BinarySerializer::write_system_def(const Config& cfg, BinarySystemDef& bin) {
    #define HPACTOR_SYSTEM_FIELD(name, type, toml, def) bin.name##_offset = write_string(to_string(cfg.name));
    #include "config/system_fields.def"
    #undef HPACTOR_SYSTEM_FIELD
}
```

Note: Binary serialization for non-string fields writes values directly; string fields convert via offset. A second X-macro or a per-field helper discriminates between the two cases. For types that need custom serialization (e.g., `OverflowPolicy`), a `to_string`/`from_string` free function is used.

### MailboxDefaults / SystemMailboxDef Merge

`MailboxDefaults` (runtime) and `SystemMailboxDef` (TOML) are the same data. After extraction, there is one `MailboxDefaults` generated from `mailbox_fields.def` and used by both the TOML parser subsystem and the runtime `ActorSystem`.

### Files Changed

- **New:** `include/hpactor/config/system_fields.def` (~35 lines)
- **New:** `include/hpactor/config/mailbox_fields.def` (~12 lines)
- **New:** `include/hpactor/config/dispatcher_fields.def` (~8 lines)
- **New:** `include/hpactor/config/actor_fields.def` (~20 lines)
- **Modified:** `include/hpactor/core/actor_system.hpp` — `Config` and `MailboxDefaults` generated from .def files
- **Modified:** `include/hpactor/config/topology_model.hpp` — `SystemDef`, `ActorDef`, `DispatcherDef`, `SystemMailboxDef` generated
- **Modified:** `include/hpactor/config/binary_format.hpp` — `BinarySystemDef`, `BinaryActorDef`, `BinaryDispatcherDef` generated
- **Modified:** `src/config/toml_parser.cpp` — field conversions generated
- **Modified:** `src/config/binary_serializer.cpp` — field serialization generated
- **Modified:** `src/config/binary_loader.cpp` — field deserialization generated

---

## Section 5: `DispatchPolicy` Enum — Deduplication

### Current State

`DispatchPolicy` is defined identically in two places, deliberately duplicated:

```cpp
// include/hpactor/sched/dispatch_policy.hpp
enum class DispatchPolicy : uint8_t { Cooperative, DedicatedThread, DedicatedPool };

// include/hpactor/config/topology_model.hpp
enum class DispatchPolicy : uint8_t { Cooperative, DedicatedThread, DedicatedPool };
```

### Action

Move the single definition to `include/hpactor/types/types.hpp` (or `adt/`). Both namespaces access it via `using` or direct reference.

```cpp
// include/hpactor/types/types.hpp  (canonical location)
enum class DispatchPolicy : uint8_t { Cooperative, DedicatedThread, DedicatedPool };

// include/hpactor/sched/dispatch_policy.hpp  (backward compat for sched consumers)
namespace hpactor::sched {
    using DispatchPolicy = hpactor::DispatchPolicy;
}
```

### Files Changed

- **Modified:** `include/hpactor/types/types.hpp` — canonical `DispatchPolicy` enum
- **Modified:** `include/hpactor/sched/dispatch_policy.hpp` — `using` alias
- **Modified:** `include/hpactor/config/topology_model.hpp` — remove dup, use `hpactor::DispatchPolicy`
- **Modified:** ~5 files that include the old duplicated header

---

## Summary

| Section | New ADT | New Files | Modified Files | Call Sites |
|---------|---------|-----------|---------------|------------|
| 1. Opaque IDs | `Id<Tag, T>` | `adt/id.hpp`, `adt/tags.hpp` | ~7 | ~5 |
| 2. Result types | (none — `result<T>` already canonical) | 0 | 2 | ~5 |
| 3. Node descriptors | `NodeIdentity` | `adt/node_identity.hpp` | ~6 | ~35 |
| 4. Config schema | X-macro tables | 4 `.def` files | ~8 | ~50 |
| 5. DispatchPolicy | (dedup) | 0 | ~7 | ~15 |

**Total:** 7 new files, ~30 modified files, ~110 call sites updated.

**New `include/hpactor/adt/` directory:**

```
include/hpactor/adt/
├── id.hpp              — Id<Tag, T> template + std::hash
├── tags.hpp            — Tag types (ActorTag, MessageTag, AlarmTag, TimerTag)
├── node_identity.hpp   — NodeIdentity struct
└── stream_buffer.hpp   — existing, unchanged
```
