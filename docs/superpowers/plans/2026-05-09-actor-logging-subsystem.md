# Actor Logging Subsystem Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a structured logging subsystem with severity levels, category filters, structured fields, async MPSC ring buffer, and multiple sinks (stderr, file, rotating file, memory) behind a compile-time `ENABLE_ACTOR_LOGGING` flag.

**Architecture:** Follows the existing metrics subsystem pattern: a lock-free MPSC ring buffer owned by `ActorSystem`, wired to producers (actors, mailboxes, scheduler, memory, network) via raw pointer. A dedicated drain thread (not an actor, to avoid recursion) consumes batches, formats, and writes to sinks. The public API is a set of `HPACTOR_LOG_*` macros that check `enabled()` before building fields to keep the disabled path cheap.

**Tech Stack:** C++20, no exceptions, no RTTI, no new external dependencies. Reuses the MPSC CAS ring buffer pattern from `metrics::MpscRingBuffer` but with runtime capacity.

**Spec:** `docs/superpowers/specs/2026-05-09-actor-logging-subsystem-design.md`

---

## File Map

### New Files (create)

```
include/hpactor/log/
    log_level.hpp           — LogLevel enum, to_string, parse_level
    log_category.hpp        — LogCategory enum, LogEventId enum, to_string, parse_category
    log_field.hpp           — LogFieldType, LogField struct, field() factory helpers
    log_event.hpp           — LogEvent struct (timestamp, level, category, actor_id, fields, etc.)
    log_config.hpp          — LogConfig, RotatingFileConfig, LogFormat, DropPolicy, LogSinkKind enums
    log_ring_buffer.hpp     — LogRingBuffer (MPSC, runtime capacity, bounded)
    log_sink.hpp            — ILogSink interface
    log_formatter.hpp       — ILogFormatter, TextLogFormatter, JsonLogFormatter
    log_drain.hpp           — LogDrain (dedicated consumer thread)
    logger.hpp              — Logger class, global_logger(), HPACTOR_LOG_* macros
    log_manager.hpp         — LogManager (owns config, ring buffer, drain, formatter, sinks)

src/log/
    log_level.cpp           — LogLevel string conversion and parsing
    log_category.cpp        — LogCategory string conversion and parsing
    log_formatter.cpp       — TextLogFormatter and JsonLogFormatter implementation
    log_drain.cpp           — LogDrain thread loop, batching, sink writes
    logger.cpp              — Logger::emit(), global_logger() no-op singleton
    log_manager.cpp         — LogManager construction, init, shutdown
    stderr_sink.cpp         — StderrSink implementation
    file_sink.cpp           — FileSink implementation
    rotating_file_sink.cpp  — RotatingFileSink implementation

tests/log/
    test_log_level.cpp      — Level parsing and comparison
    test_log_config.cpp     — Config defaults and category thresholds
    test_log_ring_buffer.cpp— Ring buffer push/drain/overflow/lost counter
    test_log_formatter.cpp  — Text and JSON output, JSON escaping
    test_log_sinks.cpp      — Stderr, file, rotating file, memory sink behavior
    test_log_integration.cpp— ActorSystem emit, mailbox warning, TOML config, shutdown drain
```

### Modified Files

```
CMakeLists.txt                          — ENABLE_ACTOR_LOGGING option, new sources, test registration
include/hpactor/config/hpactor_config.hpp.in — #cmakedefine01 HPACTOR_ENABLE_ACTOR_LOGGING
include/hpactor/config/topology_model.hpp    — LogConfig member in SystemDef
src/config/toml_parser.cpp                   — Parse [system.logging] TOML table
include/hpactor/core/actor_system.hpp        — LogManager member, logger wiring in spawn()
src/actor/actor_system.cpp                   — LogManager init/shutdown, TOML config wire
include/hpactor/actor/abstract_actor.hpp     — set_logger(void*) virtual no-op
include/hpactor/actor/event_based_actor.hpp  — set_logger(void*) override, store logger ptr
include/hpactor/mailbox/mpsc_actor_mailbox.hpp — Logger ptr for mailbox depth warnings
include/hpactor/sched/scheduler.hpp          — set_logger(void*) virtual no-op on IScheduler
src/sched/scheduler.cpp                      — HybridScheduler set_logger, dispatch/steal logs
src/actor/event_based_actor.cpp              — Actor lifecycle logs (spawn/terminate)
src/config/toml_parser.cpp                   — Config/bootstrap logs
src/mem/                                      — Memory subsystem warning/error logs
src/net/                                      — Registrar, discovery, network logs
tests/CMakeLists.txt                          — New test targets
```

---

## Phase 1: Core Types and Compile-Time Gate

### Task 1: CMake option and config define

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `include/hpactor/config/hpactor_config.hpp.in`

- [ ] **Step 1: Add ENABLE_ACTOR_LOGGING option to CMakeLists.txt**

Near the other `option()` declarations (around line 30-35), add:

```cmake
option(ENABLE_ACTOR_LOGGING "Enable structured actor logging subsystem" ON)
```

- [ ] **Step 2: Add HPACTOR_ENABLE_ACTOR_LOGGING define to hpactor_config.hpp.in**

```cpp
#cmakedefine01 HPACTOR_ENABLE_ACTOR_LOGGING
```

- [ ] **Step 3: Reconfigure and verify the define appears**

```bash
cmake -S . -B build -GNinja
grep HPACTOR_ENABLE_ACTOR_LOGGING build/include/hpactor/config/hpactor_config.hpp
```

Expected: `#define HPACTOR_ENABLE_ACTOR_LOGGING 1`

- [ ] **Step 4: Verify OFF path compiles**

```bash
cmake -S . -B build -GNinja -DENABLE_ACTOR_LOGGING=OFF
grep HPACTOR_ENABLE_ACTOR_LOGGING build/include/hpactor/config/hpactor_config.hpp
```

Expected: `#define HPACTOR_ENABLE_ACTOR_LOGGING 0`

- [ ] **Step 5: Restore default ON and commit**

```bash
cmake -S . -B build -GNinja -DENABLE_ACTOR_LOGGING=ON
git add CMakeLists.txt include/hpactor/config/hpactor_config.hpp.in
git commit -m "feat(log): add ENABLE_ACTOR_LOGGING CMake option and config define"
```

---

### Task 2: LogLevel enum

**Files:**
- Create: `include/hpactor/log/log_level.hpp`
- Create: `src/log/log_level.cpp`
- Create: `tests/log/test_log_level.cpp`
- Modify: `CMakeLists.txt` (add sources)
- Modify: `tests/CMakeLists.txt` (add test)

- [ ] **Step 1: Write the test file**

```cpp
// tests/log/test_log_level.cpp
#include <hpactor/log/log_level.hpp>
#include <gtest/gtest.h>

using namespace hpactor::log;

TEST(LogLevel, ToString) {
    EXPECT_STREQ(to_string(LogLevel::kCritical), "critical");
    EXPECT_STREQ(to_string(LogLevel::kError),    "error");
    EXPECT_STREQ(to_string(LogLevel::kWarning),  "warning");
    EXPECT_STREQ(to_string(LogLevel::kInfo),     "info");
    EXPECT_STREQ(to_string(LogLevel::kDebug),    "debug");
    EXPECT_STREQ(to_string(LogLevel::kTrace),    "trace");
    EXPECT_STREQ(to_string(LogLevel::kOff),      "off");
}

TEST(LogLevel, ParseLevel) {
    EXPECT_EQ(parse_level("critical").value(), LogLevel::kCritical);
    EXPECT_EQ(parse_level("error").value(),    LogLevel::kError);
    EXPECT_EQ(parse_level("warning").value(),  LogLevel::kWarning);
    EXPECT_EQ(parse_level("info").value(),     LogLevel::kInfo);
    EXPECT_EQ(parse_level("debug").value(),    LogLevel::kDebug);
    EXPECT_EQ(parse_level("trace").value(),    LogLevel::kTrace);
    EXPECT_EQ(parse_level("off").value(),      LogLevel::kOff);
    EXPECT_FALSE(parse_level("invalid").has_value());
}

TEST(LogLevel, Ordering) {
    // Lower numeric = higher severity
    EXPECT_LT(static_cast<uint8_t>(LogLevel::kCritical),
              static_cast<uint8_t>(LogLevel::kError));
    EXPECT_LT(static_cast<uint8_t>(LogLevel::kDebug),
              static_cast<uint8_t>(LogLevel::kTrace));
}

TEST(LogLevel, EnabledWhenLevelLEQThreshold) {
    // kInfo (3) is enabled when threshold is kDebug (4): 3 <= 4 = true
    EXPECT_LE(static_cast<uint8_t>(LogLevel::kInfo),
              static_cast<uint8_t>(LogLevel::kDebug));
    // kDebug (4) is NOT enabled when threshold is kInfo (3): 4 <= 3 = false
    EXPECT_GT(static_cast<uint8_t>(LogLevel::kDebug),
              static_cast<uint8_t>(LogLevel::kInfo));
}
```

- [ ] **Step 2: Write the header**

```cpp
// include/hpactor/log/log_level.hpp
#pragma once

#include <cstdint>
#include <string_view>
#include "hpactor/types/result.hpp"

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

- [ ] **Step 3: Write the source file**

```cpp
// src/log/log_level.cpp
#include <hpactor/log/log_level.hpp>

namespace hpactor::log {

const char* to_string(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::kCritical: return "critical";
    case LogLevel::kError:    return "error";
    case LogLevel::kWarning:  return "warning";
    case LogLevel::kInfo:     return "info";
    case LogLevel::kDebug:    return "debug";
    case LogLevel::kTrace:    return "trace";
    case LogLevel::kOff:      return "off";
    }
    return "unknown";
}

result<LogLevel> parse_level(std::string_view value) noexcept {
    if (value == "critical") return LogLevel::kCritical;
    if (value == "error")    return LogLevel::kError;
    if (value == "warning")  return LogLevel::kWarning;
    if (value == "info")     return LogLevel::kInfo;
    if (value == "debug")    return LogLevel::kDebug;
    if (value == "trace")    return LogLevel::kTrace;
    if (value == "off")      return LogLevel::kOff;
    return error("unknown log level");
}

} // namespace hpactor::log
```

- [ ] **Step 4: Add sources to CMakeLists.txt**

In the `add_library(hpactor_lib SHARED ...)` block, add `src/log/log_level.cpp`. In `tests/CMakeLists.txt`, add the test target:

```cmake
add_test_executable(test_log_level tests/log/test_log_level.cpp)
```

- [ ] **Step 5: Build and run test**

```bash
ninja -C build test_log_level && ./build/tests/test_log_level
```

Expected: 4 tests pass.

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/log/log_level.hpp src/log/log_level.cpp tests/log/test_log_level.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(log): add LogLevel enum with string conversion and parsing"
```

---

### Task 3: LogCategory and LogEventId enums

**Files:**
- Create: `include/hpactor/log/log_category.hpp`
- Create: `src/log/log_category.cpp`

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/log/log_category.hpp
#pragma once

#include <cstdint>
#include <string_view>
#include "hpactor/types/result.hpp"

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
    kCount,  // sentinel, not a valid emit category
};

// Stable numeric IDs for common framework events.
// Ranges: 1000-1099 actor, 1100-1199 mailbox, 1200-1299 memory,
//         1300-1399 registrar/discovery, 1400-1499 network, 1500-1599 scheduler
enum class LogEventId : uint32_t {
    kActorSpawned = 1000,
    kActorTerminated,
    kActorStateTransfer,
    kActorLinkRejected,
    kMailboxDepthHigh = 1100,
    kMemoryAlloc = 1200,
    kMemoryFree,
    kMemoryCorruption,
    kRegistrarRegister = 1300,
    kRegistrarResolveMiss,
    kDiscoveryNodeJoined,
    kDiscoveryNodeDead,
    kNetworkFrameReceived = 1400,
    kNetworkFrameDecodeFailed,
    kSchedulerDispatch = 1500,
    kSchedulerSteal,
};

const char* to_string(LogCategory category) noexcept;
result<LogCategory> parse_category(std::string_view value) noexcept;

} // namespace hpactor::log
```

- [ ] **Step 2: Write the source file**

```cpp
// src/log/log_category.cpp
#include <hpactor/log/log_category.hpp>

namespace hpactor::log {

const char* to_string(LogCategory category) noexcept {
    switch (category) {
    case LogCategory::kActor:       return "actor";
    case LogCategory::kActorState:  return "actor_state";
    case LogCategory::kMailbox:     return "mailbox";
    case LogCategory::kScheduler:   return "scheduler";
    case LogCategory::kMemory:      return "memory";
    case LogCategory::kRegistrar:   return "registrar";
    case LogCategory::kDiscovery:   return "discovery";
    case LogCategory::kNetwork:     return "network";
    case LogCategory::kRpc:         return "rpc";
    case LogCategory::kConfig:      return "config";
    case LogCategory::kSupervision: return "supervision";
    case LogCategory::kCli:         return "cli";
    case LogCategory::kHttp:        return "http";
    case LogCategory::kUser:        return "user";
    case LogCategory::kCount:       return "count";
    }
    return "unknown";
}

result<LogCategory> parse_category(std::string_view value) noexcept {
    if (value == "actor")       return LogCategory::kActor;
    if (value == "actor_state") return LogCategory::kActorState;
    if (value == "mailbox")     return LogCategory::kMailbox;
    if (value == "scheduler")   return LogCategory::kScheduler;
    if (value == "memory")      return LogCategory::kMemory;
    if (value == "registrar")   return LogCategory::kRegistrar;
    if (value == "discovery")   return LogCategory::kDiscovery;
    if (value == "network")     return LogCategory::kNetwork;
    if (value == "rpc")         return LogCategory::kRpc;
    if (value == "config")      return LogCategory::kConfig;
    if (value == "supervision") return LogCategory::kSupervision;
    if (value == "cli")         return LogCategory::kCli;
    if (value == "http")        return LogCategory::kHttp;
    if (value == "user")        return LogCategory::kUser;
    return error("unknown log category");
}

} // namespace hpactor::log
```

- [ ] **Step 3: Add to CMakeLists.txt and build**

```bash
# Add src/log/log_category.cpp to hpactor_lib sources in CMakeLists.txt
ninja -C build hpactor_lib
```

Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/log/log_category.hpp src/log/log_category.cpp CMakeLists.txt
git commit -m "feat(log): add LogCategory and LogEventId enums with string conversion"
```

---

### Task 4: LogField types

**Files:**
- Create: `include/hpactor/log/log_field.hpp`

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/log/log_field.hpp
#pragma once

#include <cstdint>
#include <span>

namespace hpactor::log {

inline constexpr uint8_t kMaxLogFields = 4;

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

inline LogField field(const char* name, int64_t value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kInt64;
    f.value.i64 = value;
    return f;
}

inline LogField field(const char* name, uint64_t value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kUInt64;
    f.value.u64 = value;
    return f;
}

inline LogField field(const char* name, double value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kDouble;
    f.value.f64 = value;
    return f;
}

inline LogField field(const char* name, bool value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kBool;
    f.value.boolean = value;
    return f;
}

inline LogField field_lit(const char* name, const char* value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kStringLiteral;
    f.value.str = value;
    return f;
}

inline LogField field_ptr(const char* name, const void* value) noexcept {
    LogField f{};
    f.name = name;
    f.type = LogFieldType::kPointer;
    f.value.ptr = value;
    return f;
}

} // namespace hpactor::log
```

- [ ] **Step 2: Build and commit**

```bash
ninja -C build hpactor_lib
git add include/hpactor/log/log_field.hpp
git commit -m "feat(log): add LogField types with factory helpers"
```

---

### Task 5: LogEvent struct

**Files:**
- Create: `include/hpactor/log/log_event.hpp`

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/log/log_event.hpp
#pragma once

#include <cstdint>
#include <type_traits>
#include "hpactor/log/log_level.hpp"
#include "hpactor/log/log_category.hpp"
#include "hpactor/log/log_field.hpp"
#include "hpactor/core/actor_id.hpp"

namespace hpactor::log {

struct LogEvent {
    uint64_t timestamp_ns;   // Unix epoch ns from system_clock
    LogLevel level;
    LogCategory category;
    ActorId actor_id;        // 0 = no actor context
    uint64_t trace_id;       // from TraceContext when available
    uint64_t span_id;        // from TraceContext when available
    uint32_t worker_id;      // scheduler worker, UINT32_MAX if unknown
    uint32_t type_tag;       // TypedMessage tag when available
    uint32_t event_id;       // stable numeric LogEventId
    uint32_t line;
    const char* file;
    const char* message;
    LogField fields[kMaxLogFields];
    uint8_t field_count;
};

static_assert(std::is_trivially_copyable_v<LogEvent>,
              "LogEvent must be trivially copyable for ring buffer");

} // namespace hpactor::log
```

- [ ] **Step 2: Build and commit**

```bash
# Add a static_assert check in the header or a quick compile test
ninja -C build hpactor_lib
```

```bash
git add include/hpactor/log/log_event.hpp
git commit -m "feat(log): add LogEvent struct"
```

---

### Task 6: LogConfig struct

**Files:**
- Create: `include/hpactor/log/log_config.hpp`
- Create: `tests/log/test_log_config.cpp`

- [ ] **Step 1: Write the header**

```cpp
// include/hpactor/log/log_config.hpp
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "hpactor/log/log_level.hpp"
#include "hpactor/log/log_category.hpp"

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
    uint64_t max_bytes = 104857600;  // 100 MiB
    uint32_t max_files = 5;
};

struct LogConfig {
    bool enabled = true;
    LogLevel default_level = LogLevel::kInfo;
    std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)> levels{};
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

- [ ] **Step 2: Write the test for default config and category thresholds**

```cpp
// tests/log/test_log_config.cpp
#include <hpactor/log/log_config.hpp>
#include <gtest/gtest.h>

using namespace hpactor::log;

TEST(LogConfig, Defaults) {
    LogConfig cfg{};
    EXPECT_TRUE(cfg.enabled);
    EXPECT_EQ(cfg.default_level, LogLevel::kInfo);
    EXPECT_EQ(cfg.format, LogFormat::kJson);
    EXPECT_EQ(cfg.ring_buffer_capacity, 65536);
    EXPECT_EQ(cfg.flush_on_level, LogLevel::kError);
}

TEST(LogConfig, CategoryLevelsDefaultToDefaultLevel) {
    LogConfig cfg{};
    cfg.default_level = LogLevel::kInfo;
    // levels array is zero-initialized (kCritical=0), so each category
    // needs explicit initialization. Test that the config can hold per-category levels.
    cfg.levels[static_cast<size_t>(LogCategory::kMailbox)] = LogLevel::kWarning;
    cfg.levels[static_cast<size_t>(LogCategory::kMemory)] = LogLevel::kWarning;
    cfg.levels[static_cast<size_t>(LogCategory::kNetwork)] = LogLevel::kWarning;

    EXPECT_EQ(cfg.levels[static_cast<size_t>(LogCategory::kMailbox)], LogLevel::kWarning);
    EXPECT_EQ(cfg.levels[static_cast<size_t>(LogCategory::kActor)], LogLevel::kCritical);
}
```

- [ ] **Step 3: Add test target, build, run**

```bash
# Add test_log_config to tests/CMakeLists.txt
ninja -C build test_log_config && ./build/tests/test_log_config
```

Expected: 2 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/log/log_config.hpp tests/log/test_log_config.cpp tests/CMakeLists.txt
git commit -m "feat(log): add LogConfig struct with format, sink, and threshold settings"
```

---

### Task 7: No-op macros and global_logger stub

**Files:**
- Create: `include/hpactor/log/logger.hpp`

- [ ] **Step 1: Write the logger header with macros and no-op support**

```cpp
// include/hpactor/log/logger.hpp
#pragma once

#include <array>
#include <span>
#include "hpactor/config/hpactor_config.hpp"
#include "hpactor/log/log_level.hpp"
#include "hpactor/log/log_category.hpp"
#include "hpactor/log/log_event.hpp"
#include "hpactor/log/log_field.hpp"
#include "hpactor/core/actor_id.hpp"

namespace hpactor::log {

class Logger {
public:
    Logger() = default;

    // Set by LogManager when logging is active
    void configure(class LogRingBuffer* buffer,
                   const std::array<LogLevel,
                       static_cast<size_t>(LogCategory::kCount)>* levels,
                   LogLevel flush_on_level = LogLevel::kError,
                   void (*nudge_fn)(void*) noexcept = nullptr,
                   void* nudge_ctx = nullptr) noexcept
    {
        buffer_ = buffer;
        levels_ = levels;
        flush_on_level_ = flush_on_level;
        nudge_fn_ = nudge_fn;
        nudge_ctx_ = nudge_ctx;
    }

    bool enabled(LogLevel level, LogCategory category) const noexcept {
        if (!buffer_ || !levels_) return false;
        auto idx = static_cast<size_t>(category);
        LogLevel threshold = (*levels_)[idx];
        // kOff (6) means disabled; a zero-initialized level means no override
        // so fall back to the default_level check (done via the caller passing
        // the right threshold). Here we just check the stored threshold.
        if (threshold == LogLevel::kOff) return false;
        return static_cast<uint8_t>(level) <= static_cast<uint8_t>(threshold);
    }

    void nudge() noexcept {
        if (nudge_fn_) nudge_fn_(nudge_ctx_);
    }

    void emit(LogEvent event) noexcept;

    void emit(LogLevel level,
              LogCategory category,
              ActorId actor_id,
              uint32_t event_id,
              const char* message,
              std::span<const LogField> fields,
              const char* file,
              uint32_t line) noexcept;

private:
    LogRingBuffer* buffer_ = nullptr;
    const std::array<LogLevel,
        static_cast<size_t>(LogCategory::kCount)>* levels_ = nullptr;
    LogLevel flush_on_level_ = LogLevel::kError;
    uint64_t fields_dropped_{0};  // incremented when >kMaxLogFields passed
    void (*nudge_fn_)(void*) noexcept = nullptr;
    void* nudge_ctx_ = nullptr;
};

Logger& global_logger() noexcept;

} // namespace hpactor::log

#if HPACTOR_ENABLE_ACTOR_LOGGING

#define HPACTOR_LOG_CRITICAL(category, actor_id, event_id, message, ...) \
    do { \
        auto& __hpactor_logger = ::hpactor::log::global_logger(); \
        if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kCritical, category)) { \
            ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__}; \
            __hpactor_logger.emit(::hpactor::log::LogLevel::kCritical, category, \
                                  actor_id, event_id, message, __hpactor_fields, __FILE__, __LINE__); \
        } \
    } while(0)

#define HPACTOR_LOG_ERROR(category, actor_id, event_id, message, ...) \
    do { \
        auto& __hpactor_logger = ::hpactor::log::global_logger(); \
        if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kError, category)) { \
            ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__}; \
            __hpactor_logger.emit(::hpactor::log::LogLevel::kError, category, \
                                  actor_id, event_id, message, __hpactor_fields, __FILE__, __LINE__); \
        } \
    } while(0)

#define HPACTOR_LOG_WARNING(category, actor_id, event_id, message, ...) \
    do { \
        auto& __hpactor_logger = ::hpactor::log::global_logger(); \
        if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kWarning, category)) { \
            ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__}; \
            __hpactor_logger.emit(::hpactor::log::LogLevel::kWarning, category, \
                                  actor_id, event_id, message, __hpactor_fields, __FILE__, __LINE__); \
        } \
    } while(0)

#define HPACTOR_LOG_INFO(category, actor_id, event_id, message, ...) \
    do { \
        auto& __hpactor_logger = ::hpactor::log::global_logger(); \
        if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kInfo, category)) { \
            ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__}; \
            __hpactor_logger.emit(::hpactor::log::LogLevel::kInfo, category, \
                                  actor_id, event_id, message, __hpactor_fields, __FILE__, __LINE__); \
        } \
    } while(0)

#define HPACTOR_LOG_DEBUG(category, actor_id, event_id, message, ...) \
    do { \
        auto& __hpactor_logger = ::hpactor::log::global_logger(); \
        if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kDebug, category)) { \
            ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__}; \
            __hpactor_logger.emit(::hpactor::log::LogLevel::kDebug, category, \
                                  actor_id, event_id, message, __hpactor_fields, __FILE__, __LINE__); \
        } \
    } while(0)

#define HPACTOR_LOG_TRACE(category, actor_id, event_id, message, ...) \
    do { \
        auto& __hpactor_logger = ::hpactor::log::global_logger(); \
        if (__hpactor_logger.enabled(::hpactor::log::LogLevel::kTrace, category)) { \
            ::hpactor::log::LogField __hpactor_fields[] = {__VA_ARGS__}; \
            __hpactor_logger.emit(::hpactor::log::LogLevel::kTrace, category, \
                                  actor_id, event_id, message, __hpactor_fields, __FILE__, __LINE__); \
        } \
    } while(0)

#else // HPACTOR_ENABLE_ACTOR_LOGGING disabled

#define HPACTOR_LOG_CRITICAL(category, actor_id, event_id, message, ...) ((void)0)
#define HPACTOR_LOG_ERROR(category, actor_id, event_id, message, ...)    ((void)0)
#define HPACTOR_LOG_WARNING(category, actor_id, event_id, message, ...)  ((void)0)
#define HPACTOR_LOG_INFO(category, actor_id, event_id, message, ...)     ((void)0)
#define HPACTOR_LOG_DEBUG(category, actor_id, event_id, message, ...)    ((void)0)
#define HPACTOR_LOG_TRACE(category, actor_id, event_id, message, ...)    ((void)0)

#endif // HPACTOR_ENABLE_ACTOR_LOGGING
```

- [ ] **Step 2: Commit**

```bash
git add include/hpactor/log/logger.hpp
git commit -m "feat(log): add Logger class, global_logger, and HPACTOR_LOG_* macros"
```

---

## Phase 2: Ring Buffer, Drain, and Core Pipeline

### Task 8: LogRingBuffer

**Files:**
- Create: `include/hpactor/log/log_ring_buffer.hpp`
- Create: `tests/log/test_log_ring_buffer.cpp`

- [ ] **Step 1: Write the test**

```cpp
// tests/log/test_log_ring_buffer.cpp
#include <hpactor/log/log_ring_buffer.hpp>
#include <hpactor/log/log_event.hpp>
#include <gtest/gtest.h>
#include <thread>
#include <vector>
#include <atomic>

using namespace hpactor::log;

TEST(LogRingBuffer, PushAndDrain) {
    LogRingBuffer rb(64);
    EXPECT_TRUE(rb.empty());
    EXPECT_EQ(rb.size(), 0);

    LogEvent evt{};
    evt.level = LogLevel::kInfo;
    evt.message = "hello";
    EXPECT_TRUE(rb.try_push(evt));
    EXPECT_FALSE(rb.empty());
    EXPECT_EQ(rb.size(), 1);

    int count = 0;
    rb.drain([&](const LogEvent& e) {
        EXPECT_EQ(e.level, LogLevel::kInfo);
        EXPECT_STREQ(e.message, "hello");
        count++;
    });
    EXPECT_EQ(count, 1);
    EXPECT_TRUE(rb.empty());
}

TEST(LogRingBuffer, OverflowDropsAndCounts) {
    LogRingBuffer rb(4);
    LogEvent evt{};

    // Fill the buffer
    for (int i = 0; i < 4; i++) {
        EXPECT_TRUE(rb.try_push(evt));
    }

    // This one should drop
    EXPECT_FALSE(rb.try_push(evt));
    EXPECT_GT(rb.events_lost(), 0);
}

TEST(LogRingBuffer, ConcurrentProducers) {
    LogRingBuffer rb(1024);
    std::atomic<int> total_pushed{0};
    std::atomic<int> total_dropped{0};

    auto producer = [&]() {
        LogEvent evt{};
        for (int i = 0; i < 10000; i++) {
            if (rb.try_push(evt)) {
                total_pushed.fetch_add(1, std::memory_order_relaxed);
            } else {
                total_dropped.fetch_add(1, std::memory_order_relaxed);
            }
        }
    };

    std::thread t1(producer);
    std::thread t2(producer);
    std::thread t3(producer);
    t1.join();
    t2.join();
    t3.join();

    int drained = 0;
    rb.drain([&](const LogEvent&) { drained++; });

    EXPECT_EQ(total_pushed.load(), drained);
    EXPECT_GE(rb.events_lost(), 0);
    EXPECT_EQ(rb.events_lost(), static_cast<uint64_t>(total_dropped.load()));
}

TEST(LogRingBuffer, CapacityMustBePowerOfTwo) {
    EXPECT_THROW(LogRingBuffer(3), std::invalid_argument);
    EXPECT_NO_THROW(LogRingBuffer(4));
    EXPECT_NO_THROW(LogRingBuffer(64));
    EXPECT_NO_THROW(LogRingBuffer(65536));
}
```

- [ ] **Step 2: Write the header (runtime capacity MPSC ring buffer)**

```cpp
// include/hpactor/log/log_ring_buffer.hpp
#pragma once

#include <cstdint>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <vector>
#include "hpactor/log/log_event.hpp"

namespace hpactor::log {

class LogRingBuffer {
public:
    explicit LogRingBuffer(size_t capacity)
        : capacity_(capacity)
        , mask_(capacity - 1)
    {
        if (capacity == 0 || (capacity & (capacity - 1)) != 0) {
            throw std::invalid_argument("LogRingBuffer capacity must be a power of two");
        }
        buffer_ = std::make_unique<LogEvent[]>(capacity);
    }

    bool try_push(const LogEvent& value) noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_relaxed);
        uint64_t r = read_cursor_.load(std::memory_order_acquire);

        if (w - r >= capacity_) {
            events_lost_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // CAS-claim next write slot
        while (!write_cursor_.compare_exchange_weak(w, w + 1,
                    std::memory_order_acquire,
                    std::memory_order_relaxed)) {
            // Re-check capacity on CAS failure
            r = read_cursor_.load(std::memory_order_acquire);
            if (w - r >= capacity_) {
                events_lost_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }
        }

        buffer_[w & mask_] = value;
        std::atomic_thread_fence(std::memory_order_release);
        return true;
    }

    template <typename Fn>
    size_t drain(Fn&& callback) {
        uint64_t r = read_cursor_.load(std::memory_order_relaxed);
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_acquire);

        size_t count = 0;
        while (r < w) {
            callback(buffer_[r & mask_]);
            ++r;
            ++count;
        }

        read_cursor_.store(r, std::memory_order_release);
        return count;
    }

    uint64_t events_lost() const noexcept {
        return events_lost_.load(std::memory_order_relaxed);
    }

    size_t size() const noexcept {
        uint64_t w = write_cursor_.load(std::memory_order_acquire);
        uint64_t r = read_cursor_.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }

    bool empty() const noexcept {
        return size() == 0;
    }

private:
    size_t capacity_;
    size_t mask_;
    std::unique_ptr<LogEvent[]> buffer_;

    alignas(64) std::atomic<uint64_t> write_cursor_{0};
    alignas(64) std::atomic<uint64_t> read_cursor_{0};
    alignas(64) std::atomic<uint64_t> events_lost_{0};
};

} // namespace hpactor::log
```

- [ ] **Step 3: Build and run test**

```bash
# Add src/log sources to CMakeLists.txt, add test_log_ring_buffer to tests/CMakeLists.txt
ninja -C build test_log_ring_buffer && ./build/tests/test_log_ring_buffer
```

Expected: 4 tests pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/log/log_ring_buffer.hpp tests/log/test_log_ring_buffer.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(log): add LogRingBuffer with MPSC lock-free bounded ring"
```

---

### Task 9: ILogSink interface and MemorySink

**Files:**
- Create: `include/hpactor/log/log_sink.hpp`
- Create: `src/log/memory_sink.cpp` (or inline in header for tests)

- [ ] **Step 1: Write the sink header with factory declarations**

```cpp
// include/hpactor/log/log_sink.hpp
#pragma once

#include <string_view>
#include <vector>
#include <mutex>
#include <memory>
#include "hpactor/types/result.hpp"

namespace hpactor::log {

struct LogConfig;
struct RotatingFileConfig;

class ILogSink {
public:
    virtual ~ILogSink() = default;
    virtual result<void> write(std::string_view line) noexcept = 0;
    virtual result<void> flush() noexcept = 0;
};

// In-memory sink for tests
class MemorySink : public ILogSink {
public:
    result<void> write(std::string_view line) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.emplace_back(line);
        return {};
    }

    result<void> flush() noexcept override { return {}; }

    std::vector<std::string> lines() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

// Factory functions (implemented in respective .cpp files)
std::unique_ptr<ILogSink> make_stderr_sink();
std::unique_ptr<ILogSink> make_file_sink(const std::string& path);
std::unique_ptr<ILogSink> make_rotating_file_sink(const RotatingFileConfig& cfg);

} // namespace hpactor::log
```

- [ ] **Step 2: Build and commit**

```bash
ninja -C build hpactor_lib
git add include/hpactor/log/log_sink.hpp
git commit -m "feat(log): add ILogSink interface and MemorySink for testing"
```

---

### Task 10: LogFormatter (text and JSON)

**Files:**
- Create: `include/hpactor/log/log_formatter.hpp`
- Create: `src/log/log_formatter.cpp`
- Create: `tests/log/test_log_formatter.cpp`

- [ ] **Step 1: Write the test**

```cpp
// tests/log/test_log_formatter.cpp
#include <hpactor/log/log_formatter.hpp>
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_field.hpp>
#include <gtest/gtest.h>
#include <chrono>

using namespace hpactor::log;

// Helper to create a test event
LogEvent make_event() {
    LogEvent evt{};
    evt.timestamp_ns = 1746789123456789000ULL;  // fixed timestamp
    evt.level = LogLevel::kWarning;
    evt.category = LogCategory::kMailbox;
    evt.actor_id = ActorId{42};
    evt.event_id = 1100;
    evt.message = "mailbox depth high";
    evt.fields[0] = field("depth", uint64_t(2048));
    evt.fields[1] = field("threshold", uint64_t(1024));
    evt.field_count = 2;
    evt.worker_id = UINT32_MAX;
    evt.line = 100;
    evt.file = "mailbox.cpp";
    return evt;
}

TEST(LogFormatter, TextFormat) {
    TextLogFormatter fmt;
    std::string out;
    fmt.format(make_event(), out);

    // Should contain key fields
    EXPECT_NE(out.find("warning"), std::string::npos);
    EXPECT_NE(out.find("mailbox"), std::string::npos);
    EXPECT_NE(out.find("mailbox depth high"), std::string::npos);
    EXPECT_NE(out.find("depth"), std::string::npos);
    EXPECT_NE(out.find("2048"), std::string::npos);
    EXPECT_NE(out.find("threshold"), std::string::npos);
    EXPECT_NE(out.find("1024"), std::string::npos);
}

TEST(LogFormatter, JsonFormat) {
    JsonLogFormatter fmt;
    std::string out;
    fmt.format(make_event(), out);

    // Should contain JSON fields
    EXPECT_NE(out.find("\"level\""), std::string::npos);
    EXPECT_NE(out.find("\"warning\""), std::string::npos);
    EXPECT_NE(out.find("\"category\""), std::string::npos);
    EXPECT_NE(out.find("\"mailbox\""), std::string::npos);
    EXPECT_NE(out.find("\"actor_id\""), std::string::npos);
    EXPECT_NE(out.find("42"), std::string::npos);
    EXPECT_NE(out.find("\"depth\""), std::string::npos);
    EXPECT_NE(out.find("2048"), std::string::npos);
}

TEST(LogFormatter, JsonEscapesSpecialChars) {
    JsonLogFormatter fmt;
    LogEvent evt{};
    evt.timestamp_ns = 0;
    evt.level = LogLevel::kInfo;
    evt.category = LogCategory::kUser;
    evt.message = "hello \"world\"\nbackslash\\here";
    evt.fields[0] = field_lit("key", "val\"ue");
    evt.field_count = 1;
    evt.worker_id = UINT32_MAX;

    std::string out;
    fmt.format(evt, out);

    // Should not contain raw quotes in message
    EXPECT_EQ(out.find("\\\""), std::string::npos) << "Raw quotes should be escaped";
    EXPECT_NE(out.find("world"), std::string::npos);
    EXPECT_NE(out.find("\\n"), std::string::npos);
    EXPECT_NE(out.find("\\\\"), std::string::npos);
}

TEST(LogFormatter, EmptyFieldsOmitted) {
    TextLogFormatter fmt;
    LogEvent evt{};
    evt.timestamp_ns = 0;
    evt.level = LogLevel::kInfo;
    evt.category = LogCategory::kUser;
    evt.message = "simple message";
    evt.field_count = 0;
    evt.worker_id = UINT32_MAX;

    std::string out;
    fmt.format(evt, out);

    EXPECT_NE(out.find("simple message"), std::string::npos);
}
```

- [ ] **Step 2: Write the formatter header**

```cpp
// include/hpactor/log/log_formatter.hpp
#pragma once

#include <string>

namespace hpactor::log {

struct LogEvent;

class ILogFormatter {
public:
    virtual ~ILogFormatter() = default;
    virtual void format(const LogEvent& event, std::string& out) = 0;
};

class TextLogFormatter : public ILogFormatter {
public:
    void format(const LogEvent& event, std::string& out) override;
};

class JsonLogFormatter : public ILogFormatter {
public:
    void format(const LogEvent& event, std::string& out) override;
};

} // namespace hpactor::log
```

- [ ] **Step 3: Write the formatter source**

Key implementation details for `src/log/log_formatter.cpp`:
- `TextLogFormatter::format()`: Print ISO 8601 timestamp, level string, category string, actor_id (if non-zero), event_id (if non-zero), message, then each field as `name=value`.
- `JsonLogFormatter::format()`: Build JSON object with `ts`, `level`, `category`, `actor_id`, `event_id`, `message`, plus each field. Escape `"`, `\`, and control chars in strings.
- Timestamp formatting: Use `std::chrono::system_clock::to_time_t()` and `gmtime` for date, append nanoseconds.

- [ ] **Step 4: Build and run test**

```bash
ninja -C build test_log_formatter && ./build/tests/test_log_formatter
```

Expected: 4 tests pass.

- [ ] **Step 5: Commit**

```bash
git add include/hpactor/log/log_formatter.hpp src/log/log_formatter.cpp tests/log/test_log_formatter.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(log): add TextLogFormatter and JsonLogFormatter"
```

---

### Task 11: Logger implementation

**Files:**
- Create: `src/log/logger.cpp`
- Modify: `include/hpactor/log/logger.hpp` (add LogRingBuffer forward decl dependency)

- [ ] **Step 1: Write the logger source**

```cpp
// src/log/logger.cpp
#include <hpactor/log/logger.hpp>
#include <hpactor/log/log_ring_buffer.hpp>
#include <chrono>

namespace hpactor::log {

void Logger::emit(LogEvent event) noexcept {
    if (!buffer_) return;

    auto now = std::chrono::system_clock::now();
    event.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());

    buffer_->try_push(event);

    // Nudge drain for events at or above flush_on_level
    if (static_cast<uint8_t>(event.level) <= static_cast<uint8_t>(flush_on_level_)) {
        nudge();
    }
}

void Logger::emit(LogLevel level,
                  LogCategory category,
                  ActorId actor_id,
                  uint32_t event_id,
                  const char* message,
                  std::span<const LogField> fields,
                  const char* file,
                  uint32_t line) noexcept
{
    if (!buffer_) return;

    LogEvent evt{};
    evt.level = level;
    evt.category = category;
    evt.actor_id = actor_id;
    evt.event_id = event_id;
    evt.message = message;
    evt.file = file;
    evt.line = line;
    evt.worker_id = UINT32_MAX;
    evt.field_count = static_cast<uint8_t>(
        std::min(fields.size(), static_cast<size_t>(kMaxLogFields)));
    if (fields.size() > kMaxLogFields) {
        fields_dropped_ += fields.size() - kMaxLogFields;
    }
    for (uint8_t i = 0; i < evt.field_count; ++i) {
        evt.fields[i] = fields[i];
    }

    auto now = std::chrono::system_clock::now();
    evt.timestamp_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            now.time_since_epoch()).count());

    buffer_->try_push(evt);

    // Nudge drain for events at or above flush_on_level
    if (static_cast<uint8_t>(level) <= static_cast<uint8_t>(flush_on_level_)) {
        nudge();
    }
}

namespace {
    Logger g_noop_logger;  // buffer_ is null, all operations are no-ops
}

Logger& global_logger() noexcept {
    return g_noop_logger;
}

} // namespace hpactor::log
```

- [ ] **Step 2: Add to CMakeLists.txt, build**

```bash
ninja -C build hpactor_lib
```

Expected: Clean build.

- [ ] **Step 3: Commit**

```bash
git add src/log/logger.cpp CMakeLists.txt
git commit -m "feat(log): add Logger implementation with timestamp capture and no-op global_logger"
```

---

### Task 12: LogDrain (consumer thread)

**Files:**
- Create: `include/hpactor/log/log_drain.hpp`
- Create: `src/log/log_drain.cpp`

- [ ] **Step 1: Write the drain header**

```cpp
// include/hpactor/log/log_drain.hpp
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace hpactor::log {

class LogRingBuffer;
class ILogFormatter;
class ILogSink;
struct LogConfig;

class LogDrain {
public:
    LogDrain(LogRingBuffer& buffer,
             ILogFormatter& formatter,
             std::vector<std::unique_ptr<ILogSink>> sinks,
             const LogConfig& config) noexcept;

    ~LogDrain();

    LogDrain(const LogDrain&) = delete;
    LogDrain& operator=(const LogDrain&) = delete;

    void start();
    void stop() noexcept;
    void nudge() noexcept;  // wake for high-severity events

    uint64_t sink_errors() const noexcept { return sink_errors_.load(); }

private:
    void run();

    LogRingBuffer& buffer_;
    ILogFormatter& formatter_;
    std::vector<std::unique_ptr<ILogSink>> sinks_;
    const LogConfig& config_;

    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> wake_{false};
    std::atomic<uint64_t> sink_errors_{0};
};

} // namespace hpactor::log
```

- [ ] **Step 2: Write the drain source**

Key implementation details for `src/log/log_drain.cpp`:
- `start()`: Set `running_ = true`, launch `thread_` with `run()`.
- `run()`: Loop while `running_`. Sleep with `std::this_thread::sleep_for(~100ms)`. On wake or timeout, call `buffer_.drain()`. For each event, call `formatter_.format(event, buffer_str)`, then write to all sinks. If event level >= `config_.flush_on_level`, call `sink->flush()`. Track sink errors in `sink_errors_`.
- `stop()`: Set `running_ = false`, nudge, join thread. Final drain and flush.
- `nudge()`: Set `wake_ = true`.

- [ ] **Step 3: Build**

```bash
ninja -C build hpactor_lib
```

Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/log/log_drain.hpp src/log/log_drain.cpp CMakeLists.txt
git commit -m "feat(log): add LogDrain consumer thread with batching and sink writes"
```

---

### Task 13: LogManager

**Files:**
- Create: `include/hpactor/log/log_manager.hpp`
- Create: `src/log/log_manager.cpp`

- [ ] **Step 1: Write the manager header**

```cpp
// include/hpactor/log/log_manager.hpp
#pragma once

#include <memory>
#include <vector>
#include "hpactor/log/log_config.hpp"

namespace hpactor::log {

class LogRingBuffer;
class LogDrain;
class ILogFormatter;
class ILogSink;
class Logger;

class LogManager {
public:
    explicit LogManager(const LogConfig& config);
    ~LogManager();

    LogManager(const LogManager&) = delete;
    LogManager& operator=(const LogManager&) = delete;

    void start();
    void stop() noexcept;

    Logger& logger() noexcept { return *logger_; }
    LogRingBuffer& ring_buffer() noexcept { return *ring_buffer_; }
    const LogConfig& config() const noexcept { return config_; }

    uint64_t events_lost() const noexcept;
    uint64_t sink_errors() const noexcept;

private:
    LogConfig config_;
    std::unique_ptr<LogRingBuffer> ring_buffer_;
    std::unique_ptr<ILogFormatter> formatter_;
    std::vector<std::unique_ptr<ILogSink>> sinks_;
    std::unique_ptr<LogDrain> drain_;
    std::unique_ptr<Logger> logger_;
};

} // namespace hpactor::log
```

- [ ] **Step 2: Write the manager source**

Key implementation details for `src/log/log_manager.cpp`:
- Constructor: Validate config (capacity power-of-two). Initialize per-category level thresholds: copy from `config_.levels` and overlay `default_level` for any category still at zero (`kCritical`). Noisy categories (kMailbox, kMemory, kNetwork, kActorState, kScheduler) default to `warning` unless explicitly configured.
  - Create `LogRingBuffer`.
  - Create formatter based on `config_.format` (TextLogFormatter or JsonLogFormatter).
  - Create sinks based on `config_.sinks` entries (StderrSink, FileSink, RotatingFileSink).
  - Create `LogDrain` with ring buffer, formatter, sinks, config.
  - Call `global_logger().configure(buffer, &resolved_levels, config_.flush_on_level, nudge_fn, drain)` to install this logger as the global instance. All `HPACTOR_LOG_*` macros call `global_logger()`, so this wires every log site to the active ring buffer.
- `start()`: Call `drain_->start()`.
- `stop()`: Call `drain_->stop()`.
- Destructor: Call `stop()`.

- [ ] **Step 3: Build**

```bash
ninja -C build hpactor_lib
```

Expected: Clean build.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/log/log_manager.hpp src/log/log_manager.cpp CMakeLists.txt
git commit -m "feat(log): add LogManager owning config, ring buffer, drain, and sinks"
```

---

## Phase 3: Sinks

### Task 14: StderrSink

**Files:**
- Create: `src/log/stderr_sink.cpp`
- Create: `tests/log/test_log_sinks.cpp`

- [ ] **Step 1: Write StderrSink**

```cpp
// src/log/stderr_sink.cpp
#include <hpactor/log/log_sink.hpp>
#include <cstdio>
#include <unistd.h>

namespace hpactor::log {

class StderrSink : public ILogSink {
public:
    result<void> write(std::string_view line) noexcept override {
        // writev-style: single write with trailing newline
        std::fwrite(line.data(), 1, line.size(), stderr);
        std::fputc('\n', stderr);
        return {};
    }

    result<void> flush() noexcept override {
        std::fflush(stderr);
        return {};
    }
};

std::unique_ptr<ILogSink> make_stderr_sink() {
    return std::make_unique<StderrSink>();
}

} // namespace hpactor::log
```

- [ ] **Step 2: Write the sink test file**

```cpp
// tests/log/test_log_sinks.cpp
#include <hpactor/log/log_sink.hpp>
#include <gtest/gtest.h>

using namespace hpactor::log;

TEST(MemorySink, StoresLines) {
    MemorySink sink;
    EXPECT_TRUE(sink.write("line one").has_value());
    EXPECT_TRUE(sink.write("line two").has_value());

    auto lines = sink.lines();
    ASSERT_EQ(lines.size(), 2);
    EXPECT_EQ(lines[0], "line one");
    EXPECT_EQ(lines[1], "line two");
}

TEST(MemorySink, FlushIsNoop) {
    MemorySink sink;
    EXPECT_TRUE(sink.flush().has_value());
}

TEST(MemorySink, ClearEmptiesLines) {
    MemorySink sink;
    sink.write("test");
    sink.clear();
    EXPECT_TRUE(sink.lines().empty());
}
```

- [ ] **Step 3: Add to build, run tests**

```bash
ninja -C build test_log_sinks && ./build/tests/test_log_sinks
```

Expected: 3 tests pass.

- [ ] **Step 4: Commit**

```bash
git add src/log/stderr_sink.cpp tests/log/test_log_sinks.cpp CMakeLists.txt tests/CMakeLists.txt
git commit -m "feat(log): add StderrSink and MemorySink tests"
```

---

### Task 15: FileSink

**Files:**
- Create: `src/log/file_sink.cpp`

- [ ] **Step 1: Write FileSink**

```cpp
// src/log/file_sink.cpp
#include <hpactor/log/log_sink.hpp>
#include <fstream>
#include <mutex>

namespace hpactor::log {

class FileSink : public ILogSink {
public:
    explicit FileSink(const std::string& path)
        : file_(path, std::ios::app | std::ios::out) {}

    result<void> write(std::string_view line) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) return error("file sink not open");
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.put('\n');
        if (file_.fail()) return error("file sink write failed");
        return {};
    }

    result<void> flush() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) file_.flush();
        return {};
    }

private:
    std::ofstream file_;
    std::mutex mutex_;
};

std::unique_ptr<ILogSink> make_file_sink(const std::string& path) {
    return std::make_unique<FileSink>(path);
}

} // namespace hpactor::log
```

- [ ] **Step 2: Build and commit**

```bash
ninja -C build hpactor_lib
git add src/log/file_sink.cpp CMakeLists.txt
git commit -m "feat(log): add FileSink for appending to file"
```

---

### Task 16: RotatingFileSink

**Files:**
- Create: `src/log/rotating_file_sink.cpp`

- [ ] **Step 1: Write RotatingFileSink**

```cpp
// src/log/rotating_file_sink.cpp
#include <hpactor/log/log_sink.hpp>
#include <hpactor/log/log_config.hpp>
#include <fstream>
#include <mutex>
#include <cstdio>

namespace hpactor::log {

class RotatingFileSink : public ILogSink {
public:
    RotatingFileSink(const RotatingFileConfig& cfg)
        : cfg_(cfg)
        , file_(cfg.path, std::ios::app | std::ios::out)
    {
        // Seed bytes_written_ from current file size
        if (file_.is_open()) {
            file_.seekp(0, std::ios::end);
            bytes_written_ = static_cast<uint64_t>(file_.tellp());
        }
    }

    result<void> write(std::string_view line) noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!file_.is_open()) return error("rotating file sink not open");
        file_.write(line.data(), static_cast<std::streamsize>(line.size()));
        file_.put('\n');
        if (file_.fail()) return error("rotating file sink write failed");
        bytes_written_ += line.size() + 1;

        if (bytes_written_ >= cfg_.max_bytes) {
            rotate();
        }
        return {};
    }

    result<void> flush() noexcept override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (file_.is_open()) file_.flush();
        return {};
    }

private:
    void rotate() {
        file_.close();
        // Remove oldest file if it exists
        std::string oldest = cfg_.path + "." + std::to_string(cfg_.max_files);
        std::remove(oldest.c_str());
        // Rotate: file.log.N -> file.log.(N+1), file.log -> file.log.1
        for (int i = static_cast<int>(cfg_.max_files) - 1; i >= 1; --i) {
            std::string from = cfg_.path + "." + std::to_string(i);
            std::string to   = cfg_.path + "." + std::to_string(i + 1);
            std::rename(from.c_str(), to.c_str());
        }
        std::rename(cfg_.path.c_str(), (cfg_.path + ".1").c_str());
        // Open fresh file
        file_.open(cfg_.path, std::ios::out | std::ios::trunc);
        bytes_written_ = 0;
    }

    RotatingFileConfig cfg_;
    std::ofstream file_;
    std::mutex mutex_;
    uint64_t bytes_written_ = 0;
};

std::unique_ptr<ILogSink> make_rotating_file_sink(const RotatingFileConfig& cfg) {
    return std::make_unique<RotatingFileSink>(cfg);
}

} // namespace hpactor::log
```

- [ ] **Step 2: Build and commit**

```bash
ninja -C build hpactor_lib
git add src/log/rotating_file_sink.cpp CMakeLists.txt
git commit -m "feat(log): add RotatingFileSink with size-based rotation"
```

---

## Phase 4: ActorSystem Integration and TOML Config

### Task 17: Wire LogConfig into SystemDef and TOML parser

**Files:**
- Modify: `include/hpactor/config/topology_model.hpp`
- Modify: `src/config/toml_parser.cpp`

- [ ] **Step 1: Add LogConfig member to SystemDef**

In `topology_model.hpp`, add `#include <hpactor/log/log_config.hpp>` and a `LogConfig logging;` member to `SystemDef`.

- [ ] **Step 2: Parse [system.logging] in toml_parser.cpp**

Following the existing metrics parsing pattern (`toml_parser.cpp` around lines 241-251):

```cpp
if (auto* log_node = st.get("logging")) {
    if (log_node->is_table()) {
        auto& lt = *log_node->as_table();
        data.system.logging.enabled = read_bool(lt, "enabled", true);
        data.system.logging.default_level = parse_level(
            read_string(lt, "default_level", "info")).value_or(LogLevel::kInfo);
        data.system.logging.format = parse_format(
            read_string(lt, "format", "json")).value_or(LogFormat::kJson);
        data.system.logging.ring_buffer_capacity =
            read_uint32(lt, "ring_buffer_capacity", 65536);
        data.system.logging.flush_on_level = parse_level(
            read_string(lt, "flush_on_level", "error")).value_or(LogLevel::kError);
        // Parse sinks list
        // Parse [system.logging.levels] sub-table for per-category thresholds
        // Parse [system.logging.rotating_file] sub-table
    }
}
```

- [ ] **Step 3: Build (verify no regressions in existing TOML tests)**

```bash
ninja -C build && ctest --output-on-failure
```

Expected: All previously-passing tests still pass.

- [ ] **Step 4: Commit**

```bash
git add include/hpactor/config/topology_model.hpp src/config/toml_parser.cpp
git commit -m "feat(log): wire LogConfig into SystemDef and TOML parser"
```

---

### Task 18: Wire LogManager into ActorSystem lifecycle

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add LogManager to ActorSystem**

In `actor_system.hpp`:
- Forward-declare `log::LogManager` and `log::LogConfig`
- Add `log::LogConfig logging_config_;` member
- Add `std::unique_ptr<log::LogManager> log_manager_;` member
- Add `log::Logger* logger_ = nullptr;` for quick access

- [ ] **Step 2: Initialize in constructor**

```cpp
// In ActorSystem constructor, after metrics init:
if (config_.logging.enabled) {
    log_manager_ = std::make_unique<log::LogManager>(config_.logging);
    log_manager_->start();
    logger_ = &log_manager_->logger();
    // Install as global logger
    // (need a set_global_logger or similar mechanism)
}
```

- [ ] **Step 3: Shutdown in destructor**

```cpp
// In ActorSystem destructor, before scheduler stop:
if (log_manager_) {
    log_manager_->stop();
}
```

- [ ] **Step 4: Apply TOML config in load_topology()**

```cpp
// In load_topology(), after metrics config:
config_.logging = model.system.logging;
```

- [ ] **Step 5: Build and verify**

```bash
ninja -C build && ctest --output-on-failure
```

Expected: All tests pass (logging is default-off in test configs since no [system.logging] TOML section).

- [ ] **Step 6: Commit**

```bash
git add include/hpactor/core/actor_system.hpp src/actor/actor_system.cpp
git commit -m "feat(log): wire LogManager into ActorSystem lifecycle"
```

---

## Phase 5: Framework Integration Points

### Task 19: Actor lifecycle logs

**Files:**
- Modify: `include/hpactor/core/actor_system.hpp` (spawn)
- Modify: `src/actor/event_based_actor.cpp` (terminate)

- [ ] **Step 1: Add set_logger virtual method to AbstractActor**

```cpp
// In abstract_actor.hpp
virtual void set_logger(void* logger) noexcept {}
```

- [ ] **Step 2: Override in EventBasedActor**

```cpp
// In event_based_actor.hpp
void set_logger(void* logger) noexcept override {
    logger_ = static_cast<log::Logger*>(logger);
}
log::Logger* logger_ = nullptr;
```

- [ ] **Step 3: Wire logger in ActorSystem::spawn()**

After the existing metrics wiring pattern, add logger wiring:

```cpp
if (logger_) [[unlikely]] {
    actor->set_logger(logger_);
}
```

- [ ] **Step 4: Add spawn log**

```cpp
// In ActorSystem::spawn(), after actor creation:
HPACTOR_LOG_INFO(LogCategory::kActor, id, static_cast<uint32_t>(LogEventId::kActorSpawned),
    "actor spawned", field_lit("type", type_name));
```

- [ ] **Step 5: Add terminate log**

```cpp
// In EventBasedActor::on_exit() or wherever termination is finalized:
HPACTOR_LOG_INFO(LogCategory::kActor, actor_id_,
    static_cast<uint32_t>(LogEventId::kActorTerminated),
    "actor terminated");
```

- [ ] **Step 6: Build, run tests, commit**

```bash
ninja -C build && ctest --output-on-failure
```

```bash
git add include/hpactor/actor/abstract_actor.hpp include/hpactor/actor/event_based_actor.hpp \
        include/hpactor/core/actor_system.hpp src/actor/event_based_actor.cpp
git commit -m "feat(log): add actor lifecycle logs (spawn, terminate)"
```

---

### Task 20: Config and bootstrap logs

**Files:**
- Modify: `src/config/toml_parser.cpp`
- Modify: `src/actor/actor_system.cpp`

- [ ] **Step 1: Add config parse logs**

```cpp
// In toml_parser.cpp after successful parse:
HPACTOR_LOG_INFO(LogCategory::kConfig, ActorId{0}, 0,
    "topology loaded",
    field_lit("path", file_path.c_str()));

// On parse error:
HPACTOR_LOG_ERROR(LogCategory::kConfig, ActorId{0}, 0,
    "topology parse error",
    field_lit("error", err.message().c_str()));
```

- [ ] **Step 2: Add bootstrap logs**

In `ActorSystem::load_topology()` after bootstrap:
```cpp
HPACTOR_LOG_INFO(LogCategory::kConfig, ActorId{0}, 0,
    "topology bootstrap complete",
    field("actor_count", static_cast<uint64_t>(actor_count)));
```

- [ ] **Step 3: Build and commit**

```bash
ninja -C build && ctest --output-on-failure
git add src/config/toml_parser.cpp src/actor/actor_system.cpp
git commit -m "feat(log): add config and bootstrap log events"
```

---

### Task 21: Mailbox logs

**Files:**
- Modify: `include/hpactor/mailbox/mpsc_actor_mailbox.hpp`

- [ ] **Step 1: Add set_logger to IMailbox interface and MPSCActorMailbox**

Following the metrics pattern with `void*` to avoid pulling log headers into all TU:
- Add `virtual void set_logger(void* logger) noexcept {}` to `IMailbox`
- Override in `MPSCActorMailbox` to store `log::Logger*`
- Wire from `ActorSystem::spawn()` alongside metrics wiring

- [ ] **Step 2: Add mailbox depth warning**

```cpp
// In enqueue path, after depth check:
if (depth > threshold) {
    HPACTOR_LOG_WARNING(LogCategory::kMailbox, actor_id_,
        static_cast<uint32_t>(LogEventId::kMailboxDepthHigh),
        "mailbox depth high",
        field("depth", static_cast<uint64_t>(depth)),
        field("threshold", static_cast<uint64_t>(threshold)));
}
```

- [ ] **Step 3: Build, run tests, commit**

```bash
ninja -C build && ctest --output-on-failure
git add include/hpactor/mailbox/mpsc_actor_mailbox.hpp
git commit -m "feat(log): add mailbox depth warning log"
```

---

### Task 22: Scheduler logs

**Files:**
- Modify: `include/hpactor/sched/scheduler.hpp`
- Modify: `src/sched/scheduler.cpp`

- [ ] **Step 1: Add set_logger to IScheduler and HybridScheduler**

Same `void*` pattern as actors:
- `IScheduler::set_logger(void*)` = default no-op
- `HybridScheduler::set_logger(void*)` = store pointer
- Wire from `ActorSystem` constructor

- [ ] **Step 2: Add scheduler event logs**

```cpp
// Worker start/stop at kDebug
HPACTOR_LOG_DEBUG(LogCategory::kScheduler, ActorId{0}, 0,
    "worker started", field("worker_id", worker_id));

// Dispatch at kDebug
HPACTOR_LOG_DEBUG(LogCategory::kScheduler, actor_id,
    static_cast<uint32_t>(LogEventId::kSchedulerDispatch),
    "actor dispatched", field("worker_id", worker_id));

// Work steal at kDebug
HPACTOR_LOG_DEBUG(LogCategory::kScheduler, actor_id,
    static_cast<uint32_t>(LogEventId::kSchedulerSteal),
    "work stolen", field("from_worker", victim_id),
    field("to_worker", worker_id));
```

- [ ] **Step 3: Build and commit**

```bash
ninja -C build && ctest --output-on-failure
git add include/hpactor/sched/scheduler.hpp src/sched/scheduler.cpp
git commit -m "feat(log): add scheduler event logs (dispatch, steal, worker lifecycle)"
```

---

### Task 23: Memory subsystem logs

**Files:**
- Modify: `src/mem/slab_cache.cpp` (or equivalent memory source files)

- [ ] **Step 1: Add memory warning/error logs**

Key sites:
- Allocation fallback to `malloc` → `kWarning`
- Canary mismatch → `kError`
- Guard page fault → `kError` (use emergency writer, not normal logger)
- Corruption detection → `kError`

For guard page handler, use a direct `write()` to stderr via a pre-opened fd (emergency path) since the logger uses CAS atomics which may deadlock in signal context.

- [ ] **Step 2: Add trace-level alloc/free logs (guarded, disabled by default)**

```cpp
HPACTOR_LOG_TRACE(LogCategory::kMemory, ActorId{0},
    static_cast<uint32_t>(LogEventId::kMemoryAlloc),
    "alloc", field("size", static_cast<uint64_t>(size)),
    field_ptr("ptr", ptr));
```

- [ ] **Step 3: Build, run memory tests, commit**

```bash
ninja -C build && ctest --output-on-failure -R memory
git add src/mem/*.cpp
git commit -m "feat(log): add memory subsystem warning and error logs"
```

---

### Task 24: Registrar and discovery logs

**Files:**
- Modify: `src/net/registrar_server.cpp`, `src/net/registrar_client.cpp`, `src/net/gossip_membership.cpp` (or equivalent)

- [ ] **Step 1: Add registrar logs**

- Server start/stop → `kInfo`
- Client register → `kInfo`
- Heartbeat timeout → `kWarning`
- Resolve miss → `kWarning`
- Malformed packet → `kError`

- [ ] **Step 2: Add discovery logs**

- Node joined → `kInfo`
- Node suspected → `kWarning`
- Node dead → `kError`
- Cache purge → `kDebug`

- [ ] **Step 3: Build and commit**

```bash
ninja -C build && ctest --output-on-failure -R registrar
git add src/net/*.cpp
git commit -m "feat(log): add registrar and discovery log events"
```

---

### Task 25: Network logs

**Files:**
- Modify: `src/net/tcp_transport.cpp`, `src/net/event_loop.cpp` (or equivalent network sources)

- [ ] **Step 1: Add network error logs**

- Frame decode failure → `kError`
- Protobuf parse failure → `kError`
- Connection error → `kError`
- TLS handshake failure → `kError`

- [ ] **Step 2: Add debug/trace network logs**

- Connection opened/closed → `kDebug`
- Frame sent/received (byte count, tag) → `kTrace`
- No full payload bytes in logs

- [ ] **Step 3: Build and commit**

```bash
ninja -C build && ctest --output-on-failure -R net
git add src/net/*.cpp
git commit -m "feat(log): add network processing log events"
```

---

## Phase 6: Integration Tests

### Task 26: Log subsystem integration tests

**Files:**
- Create: `tests/log/test_log_integration.cpp`

- [ ] **Step 1: Write the integration test file**

Tests to cover:
1. `ActorSystem` with logging enabled emits spawn and terminate logs when actors are created/destroyed.
2. Mailbox warning fires when configured threshold is crossed (create actor, send many messages).
3. Network decode failure emits an error log (unit-test level, mock or real).
4. TOML `[system.logging]` section populates `ActorSystem::Config::logging` from a minimal TOML file.
5. Shutdown drains buffered logs (verify no events lost after stop).
6. Disabled logging path (enabled=false) produces no events.
7. Per-category thresholds filter correctly (set kMailbox to kOff, send mailbox events, verify none emitted).

- [ ] **Step 2: Build and run integration tests**

```bash
ninja -C build test_log_integration && ./build/tests/test_log_integration
```

- [ ] **Step 3: Run full test suite to check for regressions**

```bash
ninja -C build && ctest --output-on-failure
```

Expected: All previously-passing tests + all new log tests passing.

- [ ] **Step 4: Commit**

```bash
git add tests/log/test_log_integration.cpp tests/CMakeLists.txt
git commit -m "test(log): add subsystem integration tests"
```

---

## Phase 7: Verification and Cleanup

### Task 27: Full build and test verification

- [ ] **Step 1: Clean rebuild with logging ON**

```bash
cmake -S . -B build -GNinja -DENABLE_ACTOR_LOGGING=ON
ninja -C build
ctest --output-on-failure
```

Expected: All tests pass, no new failures.

- [ ] **Step 2: Build with logging OFF**

```bash
cmake -S . -B build -GNinja -DENABLE_ACTOR_LOGGING=OFF
ninja -C build
ctest --output-on-failure
```

Expected: All tests pass, logging sources excluded.

- [ ] **Step 3: Run with ThreadSanitizer**

```bash
cmake -S . -B build-tsan -GNinja -DENABLE_TSAN=ON -DENABLE_ACTOR_LOGGING=ON
ninja -C build-tsan
ctest --output-on-failure
```

Expected: No new TSAN warnings in the ring buffer or drain path.

- [ ] **Step 4: Run stress test manually**

A quick stress test: Start an ActorSystem with trace-level logging, spawn 100 actors that each send 1000 messages in a tight loop, verify the drain thread keeps up and `events_lost` counter is reasonable.

- [ ] **Step 5: Commit any final fixes**

```bash
git commit -m "chore(log): final verification and TSAN cleanup"
```

---

## Dependency Order

```
Task 1 (CMake) ─────────────────────────────────────────────┐
    ↓                                                        │
Task 2 (LogLevel) ──→ Task 3 (LogCategory) ──→ Task 4 (LogField) ──→ Task 5 (LogEvent)
    ↓                                                        │
Task 6 (LogConfig) ──────────────────────────────────────────┤
    ↓                                                        │
Task 7 (Macros/Logger stub) ─────────────────────────────────┤
    ↓                                                        │
Task 8 (LogRingBuffer) ──────────────────────────────────────┤
    ↓                                                        │
Task 9 (ILogSink) ──→ Task 10 (Formatter) ──→ Task 11 (Logger impl) ──→ Task 12 (Drain) ──→ Task 13 (Manager)
    ↓                                                                                          ↓
Task 14 (StderrSink) ──→ Task 15 (FileSink) ──→ Task 16 (RotatingFile) ────────────────────────┤
                                                                                               ↓
                                            Task 17 (TOML config) ──→ Task 18 (ActorSystem wire)
                                                                               ↓
                                            Task 19 (Actor lifecycle) ←────────┘
                                            Task 20 (Config/bootstrap)
                                            Task 21 (Mailbox)
                                            Task 22 (Scheduler)    ← These can run in parallel
                                            Task 23 (Memory)
                                            Task 24 (Registrar)
                                            Task 25 (Network)
                                                                               ↓
                                            Task 26 (Integration tests) ←──────┘
                                                                               ↓
                                            Task 27 (Verification)
```
