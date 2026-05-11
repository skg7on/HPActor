# Log Category X-Macro Refactor Design

## 1. Summary

Refactor the `LogCategory` and `LogEventId` enum-to-string and string-to-enum
conversions in `src/log/log_category.cpp` to use the X-Macro idiom, eliminating
redundancy where each enum value is currently written in three places (enum
definition, `to_string` switch, `parse_category` if-else chain).

Issue: [#87](https://github.com/skg7on/HPActor/issues/87)

## 2. Goals

- Define each `LogCategory` entry once in an X-Macro table.
- Define each `LogEventId` entry once in an X-Macro table.
- Generate the enum definitions from the X-Macro tables.
- Generate `to_string(LogCategory)`, `to_string(LogEventId)`, and
  `parse_category(string_view)` from the same tables.
- Preserve the existing sentinel `kCount` behavior (returned by `to_string`,
  excluded from `parse_category`).
- Preserve the existing explicit integer values for `LogEventId` entries.
- No behavioral changes to any callers. The public API surface is unchanged.
- Match existing project conventions: C++20, no exceptions, no RTTI.

## 3. Non-Goals

- Adding a `parse_event_id()` function. Not requested and no current callers.
- Changing the `kCount` sentinel pattern used by `log_config.hpp`,
  `logger.hpp`, and `log_manager.hpp`.
- Refactoring other enum-to-string patterns in the codebase.

## 4. Architecture

```
include/hpactor/log/detail/log_macros.hpp  (new)
    Defines HPACTOR_LOG_CATEGORIES(X) and HPACTOR_LOG_EVENTS(X) X-Macro tables.

include/hpactor/log/log_category.hpp
    Includes log_macros.hpp. Uses the X-Macro tables to generate the enum
    definitions. Declares to_string() and parse_category() as before.

src/log/log_category.cpp
    Includes log_macros.hpp (via log_category.hpp). Uses the X-Macro tables to
    generate function bodies. Handles kCount sentinel explicitly.
```

### 4.1 X-Macro format

Each entry is `X(enum_name, "string")`. For `LogEventId`, entries also carry
an explicit numeric value: `X(enum_name, value, "string")`.

```cpp
#define HPACTOR_LOG_CATEGORIES(X) \
    X(kActor,       "actor")       \
    X(kActorState,  "actor_state") \
    X(kMailbox,     "mailbox")     \
    /* ... */

#define HPACTOR_LOG_EVENTS(X)                                          \
    X(kActorSpawned,      1000, "actor_spawned")                       \
    X(kActorTerminated,   1001, "actor_terminated")                    \
    /* ... */
```

### 4.2 Enum generation (header)

```cpp
enum class LogCategory : uint16_t {
#define HPACTOR_ENUM_VALUE(name, str) name,
    HPACTOR_LOG_CATEGORIES(HPACTOR_ENUM_VALUE)
#undef HPACTOR_ENUM_VALUE
    kCount,  // sentinel
};
```

### 4.3 to_string generation (cpp)

```cpp
[[nodiscard]] const char* to_string(LogCategory category) noexcept {
    switch (category) {
#define HPACTOR_TO_STRING_CASE(name, str) case LogCategory::name: return str;
        HPACTOR_LOG_CATEGORIES(HPACTOR_TO_STRING_CASE)
#undef HPACTOR_TO_STRING_CASE
        case LogCategory::kCount: return "count";
    }
    return "unknown";
}
```

### 4.4 parse_category generation (cpp)

```cpp
[[nodiscard]] result<LogCategory> parse_category(std::string_view value) noexcept {
#define HPACTOR_PARSE_IF(name, str) if (value == str) return result<LogCategory>::make(LogCategory::name);
    HPACTOR_LOG_CATEGORIES(HPACTOR_PARSE_IF)
#undef HPACTOR_PARSE_IF
    return result<LogCategory>::make(error(errors::unknown, "unknown log category"));
}
```

## 5. Files Changed

| File | Change |
|------|--------|
| `include/hpactor/log/detail/log_macros.hpp` | New — X-Macro tables |
| `include/hpactor/log/log_category.hpp` | Include macros header, simplify enum definitions |
| `src/log/log_category.cpp` | Replace switch/if-else bodies with X-Macro expansions |

## 6. Testing

- Existing tests that use `to_string()` and `parse_category()` must pass
  unchanged.
- No new tests required — this is a mechanical refactor with no behavioral
  change.

## 7. Rollback

Revert the three files to their prior content. No migration or data format
changes.
