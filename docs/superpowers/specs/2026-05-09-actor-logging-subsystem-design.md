# Actor Logging Subsystem Design

## 1. Summary

Add a structured logging subsystem to HPActor that gives framework and
application developers one unified API for recording the events they explicitly
choose to log: actor lifecycle, actor state transfers, mailbox state, memory
operations, registrar events, service discovery changes, scheduler decisions,
network packet processing, and application-specific events.

The subsystem is production-oriented. It provides severity levels, category
filters, structured fields, asynchronous sinks, and a cheap disabled path. It
uses a bounded lock-free MPSC ring buffer so actor, scheduler, mailbox, memory,
and network hot paths do not format strings or perform file I/O.

## 2. Goals

- Provide log levels: `critical`, `error`, `warning`, `info`, `debug`, `trace`.
- Provide a unified macro and C++ API for developer-directed logging.
- Provide fixed logging categories for HPActor subsystems.
- Support structured JSON lines and compact human-readable text.
- Avoid allocation, blocking, or formatting on hot paths.
- Allow runtime per-category level thresholds.
- Integrate with `ActorSystem::Config` and TOML topology config.
- Provide sinks for stderr, file, rotating file, and in-memory tests.
- Expose dropped-log and sink-error counters through metrics later.
- Preserve HPActor constraints: C++20, no exceptions, no RTTI, no new external
  dependencies.

## 3. Non-Goals

- Full distributed tracing implementation. Logs can carry `TraceContext`, but
  trace sampling and trace export are out of scope.
- OpenTelemetry exporter in the first implementation.
- Remote log aggregation protocol.
- Unbounded log buffering.
- Automatic inference or collection of every subsystem event. Logs appear only
  where framework or application developers place log calls.
- Logging every memory allocation, network packet, or mailbox event by default.
- Application-defined category registry in the first version. Applications use
  `LogCategory::kUser`.

## 4. Architecture

```text
Developer-chosen producer
  HPACTOR_LOG_* or Logger::emit()
        |
        v
Filter
  enabled flag, default level, category threshold, optional sampling
        |
        v
LogRingBuffer
  MPSC, bounded, fixed event slots, dropped counter
        |
        v
LogDrain
  single consumer thread, batching, formatting, sink writes
        |
        v
LogSink
  stderr, file, rotating file, memory
```

### 4.1 Ownership

`ActorSystem` owns a `log::LogManager` when logging is enabled. `LogManager`
owns the runtime config, ring buffer, drain, formatter, and sinks.

`global_logger()` returns the active logger installed by `ActorSystem`, or a
no-op logger when logging is disabled or before system initialization.

The logger pointer is passed or made available to:

- spawned actors
- actor mailboxes
- scheduler
- memory subsystem hooks
- network and registrar components
- service discovery backends

The logging data path does not send messages to a normal actor mailbox. This
avoids recursion when logging mailbox or scheduler behavior. A future `LogActor`
may provide control-plane commands, but it should not be required for event
delivery.

### 4.2 Relationship To Metrics

Metrics are framework-selected aggregated measurements. Logging is
developer-directed event recording. A metric such as mailbox depth is maintained
because the framework decided that aggregate signal is always useful. A log line
about a mailbox, scheduler decision, memory operation, registrar lookup, or
network packet exists only when a developer intentionally records that specific
event through the logging API.

This distinction keeps the logging subsystem from becoming a hidden automatic
tracer. The logger supplies a common level model, category model, ring buffer,
formatters, and sinks; the code that calls it owns the decision about what is
worth recording.

## 5. Public Headers And Sources

```text
include/hpactor/log/
    log_level.hpp
    log_category.hpp
    log_field.hpp
    log_event.hpp
    log_config.hpp
    log_ring_buffer.hpp
    log_sink.hpp
    log_formatter.hpp
    log_drain.hpp
    logger.hpp
    log_manager.hpp

src/log/
    log_level.cpp
    log_category.cpp
    log_formatter.cpp
    log_drain.cpp
    logger.cpp
    log_manager.cpp
    stderr_sink.cpp
    file_sink.cpp
    rotating_file_sink.cpp
```

Tests:

```text
tests/log/
    test_log_level.cpp
    test_log_config.cpp
    test_log_ring_buffer.cpp
    test_log_formatter.cpp
    test_log_filter.cpp
    test_log_sinks.cpp
    test_log_integration.cpp
```

## 6. Log Levels

```cpp
namespace hpactor::log {

enum class LogLevel : uint8_t {
    kCritical = 0,
    kError    = 1,
    kWarning  = 2,
    kInfo     = 3,
    kDebug    = 4,
    kTrace    = 5,
    kOff      = 6,
};

const char* to_string(LogLevel level) noexcept;
result<LogLevel> parse_level(std::string_view value) noexcept;

} // namespace hpactor::log
```

Lower numeric value means higher severity. A log is enabled when:

```cpp
level <= threshold_for(category)
```

`kOff` disables a category.

## 7. Log Categories

```cpp
namespace hpactor::log {

enum class LogCategory : uint16_t {
    kActor = 0,
    kActorState,
    kMailbox,
    kScheduler,
    kMemory,
    kRegistrar,
    kDiscovery,
    kNetwork,
    kRpc,
    kConfig,
    kSupervision,
    kCli,
    kHttp,
    kUser,
    kCount,
};

const char* to_string(LogCategory category) noexcept;
result<LogCategory> parse_category(std::string_view value) noexcept;

} // namespace hpactor::log
```

`kCount` is not a valid emitted category. It sizes fixed arrays in
`LogConfig`.

### 7.1 Event Ids

Common framework events should get stable numeric ids so production pipelines
can match events without relying on message text:

```cpp
enum class LogEventId : uint32_t {
    kActorSpawned = 1000,
    kActorTerminated,
    kActorStateTransfer,
    kMailboxDepthHigh,
    kMemoryAlloc,
    kMemoryFree,
    kMemoryCorruption,
    kRegistrarRegister,
    kRegistrarResolveMiss,
    kDiscoveryNodeJoined,
    kDiscoveryNodeDead,
    kNetworkFrameReceived,
    kNetworkFrameDecodeFailed,
    kSchedulerDispatch,
    kSchedulerSteal,
};
```

Ranges should be reserved by subsystem in the header comments, for example
`1000-1099` actor, `1100-1199` mailbox, `1200-1299` memory, `1300-1399`
registrar and discovery, `1400-1499` network, and `1500-1599` scheduler.

## 8. Log Fields

Hot-path fields must be bounded and allocation-free:

```cpp
namespace hpactor::log {

enum class LogFieldType : uint8_t {
    kInt64,
    kUInt64,
    kDouble,
    kBool,
    kStringLiteral,
    kPointer,
};

struct LogField {
    const char* name;
    LogFieldType type;
    union {
        int64_t i64;
        uint64_t u64;
        double f64;
        bool boolean;
        const char* str;
        const void* ptr;
    } value;
};

LogField field(const char* name, int64_t value) noexcept;
LogField field(const char* name, uint64_t value) noexcept;
LogField field(const char* name, double value) noexcept;
LogField field(const char* name, bool value) noexcept;
LogField field_lit(const char* name, const char* value) noexcept;
LogField field_ptr(const char* name, const void* value) noexcept;

} // namespace hpactor::log
```

The first implementation supports up to four fields per event. Extra fields are
ignored and increment a `fields_dropped` counter in the event or manager.

## 9. Log Event

```cpp
namespace hpactor::log {

inline constexpr uint8_t kMaxLogFields = 4;

struct LogEvent {
    uint64_t timestamp_ns;
    LogLevel level;
    LogCategory category;
    ActorId actor_id;
    uint64_t trace_id;
    uint64_t span_id;
    uint32_t worker_id;
    uint32_t type_tag;
    uint32_t event_id;
    uint32_t line;
    const char* file;
    const char* message;
    LogField fields[kMaxLogFields];
    uint8_t field_count;
};

} // namespace hpactor::log
```

Field semantics:

- `timestamp_ns`: Unix epoch nanoseconds from `std::chrono::system_clock`,
  captured by the producer for direct formatting into wall-clock log records.
- `actor_id`: zero means no actor context.
- `trace_id` and `span_id`: copied from current `TraceContext` when available.
- `worker_id`: current scheduler worker id when available, otherwise `UINT32_MAX`.
- `type_tag`: current `TypedMessage` tag when available, otherwise `0`.
- `event_id`: stable numeric event id for common framework events.
- `message`, `file`, field names, and string fields should be string literals in
  hot-path code.

## 10. Log Ring Buffer

The first implementation can reuse the same producer/consumer shape as
`metrics::MpscRingBuffer<T>`, but should use runtime capacity so TOML can tune
the buffer size:

```cpp
class LogRingBuffer {
public:
    explicit LogRingBuffer(size_t capacity);

    bool try_push(const LogEvent& value) noexcept;

    template <typename Fn>
    size_t drain(Fn&& callback);

    uint64_t events_lost() const noexcept;
    size_t size() const noexcept;
    bool empty() const noexcept;
};
```

The ring buffer is multi-producer, single-consumer. Producers never block. If
the buffer is full, `events_lost` increments and `try_push()` returns false.

Capacity must be a power of two. Invalid configured capacities should fail
config validation so operator mistakes are visible.

## 11. Logger API

```cpp
namespace hpactor::log {

class Logger {
public:
    bool enabled(LogLevel level, LogCategory category) const noexcept;

    void emit(LogLevel level,
              LogCategory category,
              ActorId actor_id,
              uint32_t event_id,
              const char* message,
              std::span<const LogField> fields,
              const char* file,
              uint32_t line) noexcept;

    void emit(LogEvent event) noexcept;
};

Logger& global_logger() noexcept;

} // namespace hpactor::log
```

Macro API:

```cpp
#define HPACTOR_LOG_CRITICAL(category, actor_id, message, ...)
#define HPACTOR_LOG_ERROR(category, actor_id, message, ...)
#define HPACTOR_LOG_WARNING(category, actor_id, message, ...)
#define HPACTOR_LOG_INFO(category, actor_id, message, ...)
#define HPACTOR_LOG_DEBUG(category, actor_id, message, ...)
#define HPACTOR_LOG_TRACE(category, actor_id, message, ...)
```

The macro expansion should perform `enabled()` before building the field array.

## 12. Configuration

```cpp
namespace hpactor::log {

enum class LogFormat : uint8_t {
    kText,
    kJson,
};

enum class DropPolicy : uint8_t {
    kDropNewest,
};

enum class LogSinkKind : uint8_t {
    kStderr,
    kFile,
    kRotatingFile,
};

struct RotatingFileConfig {
    std::string path;
    uint64_t max_bytes = 104857600;
    uint32_t max_files = 5;
};

struct LogConfig {
    bool enabled = true;
    LogLevel default_level = LogLevel::kInfo;
    std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)> levels;
    LogFormat format = LogFormat::kJson;
    DropPolicy drop_policy = DropPolicy::kDropNewest;
    uint32_t ring_buffer_capacity = 65536;
    LogLevel flush_on_level = LogLevel::kError;
    std::vector<LogSinkKind> sinks;
    std::string file_path;
    RotatingFileConfig rotating_file;
};

} // namespace hpactor::log
```

Default category thresholds are initialized from `default_level`, with noisy
categories lowered at construction time:

- `kMailbox`: `warning`
- `kMemory`: `warning`
- `kNetwork`: `warning`
- `kActorState`: `warning`
- `kScheduler`: `warning`

## 13. TOML Configuration

```toml
[system.logging]
enabled = true
default_level = "info"
format = "json"
sinks = ["stderr"]
file_path = "logs/hpactor.log"
ring_buffer_capacity = 65536
drop_policy = "drop_newest"
flush_on_level = "error"

[system.logging.levels]
actor = "info"
actor_state = "debug"
mailbox = "warning"
memory = "warning"
registrar = "info"
discovery = "info"
network = "warning"
scheduler = "warning"
```

`TopologyModel::SystemDef` gains `hpactor::log::LogConfig logging`, matching
the existing pattern where CLI config lives directly in the system definition.
The parser maps `[system.logging]` into that member, and topology bootstrap
copies it into `ActorSystem::Config::logging`.

Invalid level, format, sink, or category strings should return `result<T>` with
an error. The parser should not silently ignore misspelled logging keys inside
`[system.logging.levels]`.

## 14. Sinks

```cpp
namespace hpactor::log {

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual result<void> write(std::string_view line) noexcept = 0;
    virtual result<void> flush() noexcept = 0;
};

} // namespace hpactor::log
```

Initial sinks:

- `StderrSink`: writes to `stderr`, intended for local development and
  container logs.
- `FileSink`: appends to a configured path.
- `RotatingFileSink`: rotates by size and keeps `max_files`.
- `MemorySink`: stores lines in memory for tests.

Sink writes happen only on the drain thread.

## 15. Formatters

```cpp
class ILogFormatter {
public:
    virtual ~ILogFormatter() = default;
    virtual void format(const LogEvent& event, std::string& out) = 0;
};
```

`TextLogFormatter` output:

```text
2026-05-09T12:34:56.789123Z warning mailbox actor=42 event=mailbox_depth_high depth=2048 threshold=1024
```

`JsonLogFormatter` output:

```json
{"ts":"2026-05-09T12:34:56.789123Z","level":"warning","category":"mailbox","actor_id":42,"event_id":1003,"message":"mailbox depth high","depth":2048,"threshold":1024}
```

JSON strings must escape quotes, backslashes, and control characters.

## 16. Log Drain

`LogDrain` is a single consumer that owns the formatter and sink list.

Responsibilities:

- Wake periodically or when nudged by high-severity events.
- Drain batches from the ring buffer.
- Format events into a reusable `std::string` buffer.
- Write each formatted line to all sinks.
- Track sink write failures.
- Flush sinks when event level is at or above `flush_on_level`.
- Stop cleanly during `ActorSystem` shutdown.

Producer code never waits for drain completion.

## 17. Suggested Integration Points

These are recommended call sites for HPActor's own framework logs and for
applications that want to observe similar behavior. They are not automatic data
collection rules. Each emitted log requires an explicit `HPACTOR_LOG_*` macro or
`Logger::emit()` call at the relevant point in the code.

### 17.1 Actor Lifecycle

`ActorSystem::spawn()` logs:

- `info actor_spawned`
- actor id
- actor type
- dispatch policy

Actor exit paths log:

- `info actor_terminated`
- actor id
- reason code when available

Link/monitor rejection logs:

- `warning actor_link_rejected`
- source actor id
- target actor id
- reason

### 17.2 Actor State Transfer

Scheduler state transitions log at `debug`:

- `actor_state_transfer`
- actor id
- old state
- new state
- worker id

This includes transitions such as `Idle -> Ready`, `Ready -> Running`, and
`Running -> IOWaiting`.

### 17.3 Mailbox State

Mailbox warnings:

- depth over threshold
- enqueue rejected if future bounded mailbox support rejects messages

Trace logs, disabled by default:

- enqueue
- dequeue
- empty-to-nonempty wakeup

Mailbox depth itself remains a metric.

### 17.4 Memory Operation

Warning and error logs:

- allocation fallback to `malloc`
- canary mismatch
- guard page fault
- corruption event
- hibernation serialization failure

Trace logs, disabled by default:

- alloc
- free
- hibernate in/out

The memory subsystem must avoid normal logging in signal handlers. Guard page
fault handling should use an emergency writer.

### 17.5 Registrar And Discovery

Registrar logs:

- server start/stop
- client register
- heartbeat timeout
- resolve request
- resolve miss
- malformed registrar packet

Discovery logs:

- node joined
- node suspected
- node dead
- cache purge
- static route load

### 17.6 Network Packet Processing

Error logs:

- frame decode failure
- protobuf parse failure
- connection error
- TLS handshake failure

Debug and trace logs:

- connection opened/closed
- frame sent/received
- UDP packet received
- retry/backoff

Trace packet logs should record byte counts and tags, not full payload bytes by
default. Payload logging can be a future opt-in due to size and privacy risk.

### 17.7 Scheduler

Debug logs:

- worker started/stopped
- actor dispatched
- work steal
- timer fired

Trace logs:

- idle loop transitions
- failed steal attempts if explicitly enabled

### 17.8 Config And Bootstrap

Info logs:

- topology loaded
- binary topology loaded
- actor factory registered

Error logs:

- parse error
- validation failure
- unknown actor behavior

## 18. Metrics Relationship

Logging should publish these counters through the existing metrics subsystem in
a later integration step:

- `hpactor_logs_emitted_total{level,category}`
- `hpactor_logs_dropped_total{reason}`
- `hpactor_log_sink_errors_total{sink}`
- `hpactor_log_ring_buffer_depth`

This spec does not require metrics exposure in the first patch, but the
`LogManager` should maintain counters so the metrics integration is mechanical.

## 19. Compile-Time Flag

Add:

```cmake
option(ENABLE_ACTOR_LOGGING "Enable structured actor logging subsystem" ON)
```

Generate:

```cpp
#cmakedefine01 HPACTOR_ENABLE_ACTOR_LOGGING
```

When disabled, macros compile to no-ops and logging sources are either excluded
or built as inert stubs.

## 20. Error Handling And Safety

- No logging operation throws exceptions.
- Producer APIs are `noexcept`.
- Producer APIs do not allocate in the hot path.
- Ring overflow drops the newest event and increments a counter.
- Sink write failures are contained to the drain thread.
- Emergency logging is available for allocator corruption and signal-sensitive
  paths.
- Shutdown drains remaining events for a bounded time, then flushes sinks.

## 21. Testing Plan

Unit tests:

- level parse and compare semantics
- category parse and string conversion
- config defaults and category threshold overrides
- ring buffer push, drain, overflow, and lost counter
- disabled path does not enqueue
- JSON escaping
- text formatter stable output
- stderr/file/memory sink behavior
- rotating file boundary behavior

Integration tests:

- `ActorSystem` emits actor spawn/terminate logs.
- mailbox warning fires when configured threshold is crossed.
- network decode failure emits an error log.
- TOML `[system.logging]` populates `ActorSystem::Config`.
- shutdown drains buffered logs.

Stress tests:

- concurrent producers emit from multiple threads while one drain consumes.
- high-volume trace category drops without blocking producers.

## 22. Rollout Plan

1. Add core types and no-op macros behind `ENABLE_ACTOR_LOGGING`.
2. Add ring buffer, manager, drain, formatters, and test sinks.
3. Add stderr and file sinks.
4. Wire `ActorSystem::Config` and TOML parsing.
5. Add actor lifecycle and config/bootstrap logs.
6. Add mailbox, scheduler, memory, registrar, discovery, and network logs in
   small patches.
7. Add CLI and metrics integration as follow-up work.

This order provides a usable logger before touching the highest-volume hot
paths.

## 23. Open Decisions

The design chooses conservative defaults for the first implementation:

- fixed categories instead of runtime user category registration
- four structured fields per event
- drop-newest overflow policy
- no payload logging by default
- no external log pipeline exporter

These can be revisited after the core subsystem is implemented and measured.
