# TOML Parser IoC Refactor Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use `superpowers:executing-plans` to execute this plan.

**Goal:** Refactor `TomlParser` so `parse_file_data` becomes a small orchestration function, while each TOML configuration subsystem owns its own parser and self-registers through static object initialization.

**Architecture:** Introduce an inversion-of-control registry for TOML parsing. `TomlParser` keeps file loading, import resolution, template resolution, validation, and topological sort. Subsystem parsers self-register by defining a file-scope static registrar object in their own translation unit. The registry stores parser factories discovered during static initialization, then `TomlParser` snapshots and instantiates those factories during parsing.

**Tech Stack:** C++20, toml++ v3.4.0, CMake, Ninja, `hpactor::result<T>`, existing `TopologyModel`, no RTTI, exceptions isolated to TOML adapter translation units.

---

## Review Summary

`src/config/toml_parser.cpp` currently mixes several responsibilities inside `parse_file_data`:

- TOML file loading and parse error handling.
- Entrypoint versus import file rules.
- Core `[system]` scalar parsing.
- Subsystem parsing for `[system.metrics]`, `[system.logging]`, `[system.cli]`, and `[system.discovery]`.
- Import list parsing.
- Top-level topology parsing for `[[dispatcher]]`, `[template.*]`, and `[[actor]]`.

The public `TomlParser::parse()` API is still clean and should remain stable. The refactor should shrink `parse_file_data` into a coordinator and push subsystem parse details into registered parser classes.

Important constraints:

- Only `src/config/toml_parser.cpp` currently includes `toml.hpp` and has `-fexceptions`.
- The project compiles with `-fno-exceptions` and `-fno-rtti` by default.
- The AOT compiler uses `TomlParser::parse()`, so runtime and build-time behavior must stay identical.
- Existing import, template, merge, validation, and topological ordering behavior must not change.

## Target Design

### Ownership Boundaries

Keep these responsibilities in `TomlParser`:

- Open and parse TOML entrypoint files.
- Resolve imports and glob patterns.
- Merge imported and entrypoint file data.
- Resolve template inheritance.
- Validate actor ids, supervisor references, and dispatcher references.
- Topologically sort actors.
- Log final parse success or parse failure.

Move these responsibilities into registered parsers:

- Core `[system]` keys and imports.
- `[system.metrics]`.
- `[system.logging]`.
- `[system.cli]`.
- `[system.discovery]`.
- `[[dispatcher]]`.
- `[template.*]`.
- `[[actor]]`.

### Registry Shape

Add two parser categories:

- `ITomlSystemConfigParser`: parses data under the entrypoint `[system]` table into `SystemDef`.
- `ITomlDocumentConfigParser`: parses top-level document sections into `TomlFileData`.

`TomlParser` calls document parsers for every file and system parsers only for the entrypoint file.

### Static Self-Registration Shape

Each parser translation unit owns one file-scope registrar object:

```cpp
namespace {

class MetricsConfigParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.metrics";
    static constexpr int kOrder = 100;

    std::string_view name() const noexcept override { return kName; }
    int order() const noexcept override { return kOrder; }

    result<void> parse(const TomlTableView& system,
                       SystemDef& out,
                       TomlParseContext& ctx) const override;
};

const TomlSystemParserRegistration<MetricsConfigParser>
    kRegisterMetricsConfigParser;

} // namespace
```

There is no public `TomlParser::register_*` API and no central built-in registration function. Adding a new subsystem parser means adding a parser source file to the build; the parser becomes discoverable because its static registrar runs before `main()`.

### TOML Adapter Rule

Do not expose `toml++` types from public HPActor headers. Add a small `TomlTableView` wrapper so subsystem parser interfaces do not include `toml.hpp`. This keeps exception-enabled compilation limited to the TOML adapter implementation.

## Files To Add

- `include/hpactor/config/toml_table_view.hpp`
- `include/hpactor/config/toml_parse_context.hpp`
- `include/hpactor/config/toml_config_parser.hpp`
- `include/hpactor/config/toml_parser_registry.hpp`
- `include/hpactor/config/toml_file_data.hpp`
- `src/config/toml_table_view.cpp`
- `src/config/toml_parser_registry.cpp`
- `src/config/parsers/system_core_config_parser.cpp`
- `src/config/parsers/metrics_config_parser.cpp`
- `src/config/parsers/logging_config_parser.cpp`
- `src/config/parsers/cli_config_parser.cpp`
- `src/config/parsers/discovery_config_parser.cpp`
- `src/config/parsers/topology_config_parser.cpp`
- `tests/config/test_toml_parser_registry.cpp`
- `tests/data/toml/system_subsystems.toml`

## Files To Modify

- `src/config/toml_parser.cpp`
- `tests/config/test_toml_parser.cpp`
- `tests/CMakeLists.txt`
- `CMakeLists.txt`
- `docs/architecture/core/actor-toml-config-architecture.md`
- `AGENTS.md`
- `CLAUDE.md`
- `CLAUDE_MEMORY.md`

## Phase 1: Characterization Tests

Before moving code, add tests that pin current behavior for existing subsystem parsing.

### Step 1.1: Add subsystem fixture

Create `tests/data/toml/system_subsystems.toml`:

```toml
[system]
version = "1.0"
scheduler_threads = 8
max_queue_depth = 2048
default_mailbox_size = 4096
enable_network = true
tcp_port = 9555
spawn_timeout_ms = 9000
enable_http_gateway = true
http_bind_host = "127.0.0.1"
http_port = 18080
http_max_connections = 77
http_max_request_size = 123456
http_reply_timeout_ms = 4567
use_coroutines = true

[system.metrics]
enabled = false
ring_buffer_capacity = 8192
metrics_path = "/internal/metrics"

[system.logging]
enabled = true
default_level = "debug"
format = "text"
ring_buffer_capacity = 4096
flush_on_level = "warn"
file_path = "/tmp/hpactor.log"
drop_policy = "drop_newest"
sinks = ["stderr", "file", "rotating_file"]

[system.logging.levels]
config = "trace"
network = "error"

[system.logging.rotating_file]
path = "/tmp/hpactor-rotating.log"
max_bytes = 1048576
max_files = 3

[system.cli]
enabled = true
listen_path = "/tmp/hpactor-cli.sock"
tcp_port = 7001
default_format = "json"
page_size = 25

[system.discovery]
backend = "gossip"

[[dispatcher]]
name = "io"
threads = 2
cpu_affinity = [0, 1]

[[actor]]
id = "echo"
behavior = "EchoActor"
dispatcher = "io"
mailbox_capacity = 99
```

### Step 1.2: Extend `test_toml_parser`

Modify `tests/config/test_toml_parser.cpp` with:

- `test_system_subsystems()` asserting every field from `system_subsystems.toml`.
- `test_import_rejects_system_table()` writing an imported file that contains `[system]` and asserting parse failure.

Add both functions to `main()`.

### Step 1.3: Run current tests

```bash
cmake -S . -B build -GNinja
ninja -C build test_toml_parser test_binary_roundtrip
./build/tests/test_toml_parser
./build/tests/test_binary_roundtrip
```

## Phase 2: TOML View And Parse Context

### Step 2.1: Add `TomlParseContext`

Create `include/hpactor/config/toml_parse_context.hpp`:

```cpp
#pragma once

#include <hpactor/types/types.hpp>

#include <string>

namespace hpactor::config {

class TomlParseContext {
  public:
    TomlParseContext(std::string filepath, bool entrypoint) noexcept;

    const std::string& filepath() const noexcept;
    bool is_entrypoint() const noexcept;

    result<void> fail(const char* message) const;

  private:
    std::string filepath_;
    bool entrypoint_{false};
};

} // namespace hpactor::config
```

Implement it in `src/config/toml_parser_registry.cpp` or a small `src/config/toml_parse_context.cpp`. Use `errors::unknown` to preserve current result behavior, and log `message` with `LogCategory::kConfig`.

### Step 2.2: Add `TomlTableView`

Create `include/hpactor/config/toml_table_view.hpp` without including `toml.hpp`:

```cpp
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace hpactor::config {

class TomlValueView {
  public:
    enum class Kind : uint8_t {
        Missing = 0,
        String,
        Integer,
        FloatingPoint,
        Boolean,
        Array,
        Table,
    };

    TomlValueView() noexcept = default;

    Kind kind() const noexcept;
    bool is_string() const noexcept;
    bool is_integer() const noexcept;
    bool is_floating_point() const noexcept;
    bool is_boolean() const noexcept;

    std::string as_string(std::string_view fallback) const;
    int64_t as_int64(int64_t fallback) const noexcept;
    double as_double(double fallback) const noexcept;
    bool as_bool(bool fallback) const noexcept;

  private:
    friend class TomlTableView;
    explicit TomlValueView(const void* node) noexcept;

    const void* node_{nullptr};
};

class TomlTableView {
  public:
    using StringArrayVisitor = std::function<void(std::string_view)>;
    using TableArrayVisitor = std::function<void(TomlTableView)>;
    using NamedTableVisitor =
        std::function<void(std::string_view, TomlTableView)>;
    using KeyValueVisitor =
        std::function<void(std::string_view, TomlValueView)>;

    TomlTableView() noexcept = default;

    bool valid() const noexcept;
    bool contains(std::string_view key) const;
    TomlValueView value(std::string_view key) const;
    TomlTableView table(std::string_view key) const;

    std::string read_string(std::string_view key,
                            std::string_view fallback = "") const;
    uint32_t read_uint32(std::string_view key,
                         uint32_t fallback = 0) const noexcept;
    bool read_bool(std::string_view key, bool fallback = false) const noexcept;

    void for_each_string_array(std::string_view key,
                               const StringArrayVisitor& visitor) const;
    void for_each_table_array(std::string_view key,
                              const TableArrayVisitor& visitor) const;
    void for_each_subtable(std::string_view key,
                           const NamedTableVisitor& visitor) const;
    void for_each_key_value(std::string_view key,
                            const KeyValueVisitor& visitor) const;

  private:
    friend TomlTableView make_toml_table_view(const void* table) noexcept;
    explicit TomlTableView(const void* table) noexcept;

    const void* table_{nullptr};
};

TomlTableView make_toml_table_view(const void* table) noexcept;

} // namespace hpactor::config
```

Implement `src/config/toml_table_view.cpp` with `#include <toml.hpp>`. Cast the opaque pointers to `const toml::table*` and `const toml::node*` only in this source file.

### Step 2.3: Isolate exception-enabled sources

Modify `CMakeLists.txt`:

```cmake
set(HPACTOR_TOML_EXCEPTION_SOURCES
    src/config/toml_parser.cpp
    src/config/toml_table_view.cpp
)

set_source_files_properties(
    ${HPACTOR_TOML_EXCEPTION_SOURCES}
    PROPERTIES COMPILE_FLAGS "-fexceptions"
)
```

Remove the old single-file exception block for `src/config/toml_parser.cpp`.

## Phase 3: Parser Interfaces And Registry

### Step 3.1: Add shared file data

Create `include/hpactor/config/toml_file_data.hpp`:

```cpp
#pragma once

#include <hpactor/config/topology_model.hpp>

#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::config {

struct TomlRawActor {
    ActorDef def;
    std::string inherits;
};

struct TomlFileData {
    SystemDef system;
    std::vector<DispatcherDef> dispatchers;
    std::vector<TomlRawActor> actors;
    std::unordered_map<std::string, ActorDef> templates;
};

} // namespace hpactor::config
```

This replaces the private `RawActor` and `FileData` structs in `toml_parser.cpp`.

### Step 3.2: Add parser interfaces

Create `include/hpactor/config/toml_config_parser.hpp`:

```cpp
#pragma once

#include <hpactor/config/toml_file_data.hpp>
#include <hpactor/config/toml_parse_context.hpp>
#include <hpactor/config/toml_table_view.hpp>
#include <hpactor/types/types.hpp>

#include <string_view>

namespace hpactor::config {

class ITomlSystemConfigParser {
  public:
    virtual ~ITomlSystemConfigParser() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual int order() const noexcept = 0;
    virtual result<void> parse(const TomlTableView& system,
                               SystemDef& out,
                               TomlParseContext& ctx) const = 0;
};

class ITomlDocumentConfigParser {
  public:
    virtual ~ITomlDocumentConfigParser() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual int order() const noexcept = 0;
    virtual result<void> parse(const TomlTableView& root,
                               TomlFileData& out,
                               TomlParseContext& ctx) const = 0;
};

} // namespace hpactor::config
```

### Step 3.3: Add registry

Create `include/hpactor/config/toml_parser_registry.hpp`:

```cpp
#pragma once

#include <hpactor/config/toml_config_parser.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor::config {

class TomlParserRegistry {
  public:
    using SystemParserFactory =
        std::unique_ptr<ITomlSystemConfigParser> (*)();
    using DocumentParserFactory =
        std::unique_ptr<ITomlDocumentConfigParser> (*)();

    static TomlParserRegistry& instance();

    bool add_system_parser(std::string_view name,
                           int order,
                           SystemParserFactory factory);
    bool add_document_parser(std::string_view name,
                             int order,
                             DocumentParserFactory factory);

    std::vector<std::unique_ptr<ITomlSystemConfigParser>>
    create_system_parsers() const;

    std::vector<std::unique_ptr<ITomlDocumentConfigParser>>
    create_document_parsers() const;

  private:
    TomlParserRegistry() = default;

    struct SystemEntry {
        std::string name;
        int order{0};
        SystemParserFactory factory{nullptr};
    };

    struct DocumentEntry {
        std::string name;
        int order{0};
        DocumentParserFactory factory{nullptr};
    };

    std::vector<SystemEntry> system_parsers_;
    std::vector<DocumentEntry> document_parsers_;
};

template <typename ParserT>
class TomlSystemParserRegistration {
  public:
    TomlSystemParserRegistration() {
        registered_ = TomlParserRegistry::instance().add_system_parser(
            ParserT::kName,
            ParserT::kOrder,
            []() -> std::unique_ptr<ITomlSystemConfigParser> {
                return std::make_unique<ParserT>();
            });
    }

    bool registered() const noexcept { return registered_; }

  private:
    bool registered_{false};
};

template <typename ParserT>
class TomlDocumentParserRegistration {
  public:
    TomlDocumentParserRegistration() {
        registered_ = TomlParserRegistry::instance().add_document_parser(
            ParserT::kName,
            ParserT::kOrder,
            []() -> std::unique_ptr<ITomlDocumentConfigParser> {
                return std::make_unique<ParserT>();
            });
    }

    bool registered() const noexcept { return registered_; }

  private:
    bool registered_{false};
};

} // namespace hpactor::config
```

Implement `src/config/toml_parser_registry.cpp` with:

- A function-local registry singleton allocated with `new` and intentionally kept alive until process exit, avoiding static destruction ordering issues.
- A mutex around registration and factory snapshots.
- Duplicate `name()` rejection within each parser category.
- Sorting by `order()` then `name()` before factory instantiation.
- `create_system_parsers()` and `create_document_parsers()` that return fresh parser instances for each parse.

### Step 3.4: Keep `TomlParser` public API unchanged

Do not add public registration methods to `TomlParser`. Its public header remains a parse entrypoint:

```cpp
class TomlParser {
  public:
    static result<TopologyModel> parse(const std::string& entrypoint_path);
};
```

Subsystem extension is achieved by linking a translation unit that defines a parser class and a file-scope `TomlSystemParserRegistration<T>` or `TomlDocumentParserRegistration<T>` object.

## Phase 4: Extract Built-In Parsers

### Step 4.1: Core system parser

Add `src/config/parsers/system_core_config_parser.cpp`.

Parser name: `system.core`.

Order: `0`.

Responsibilities:

- `version`
- `scheduler_threads`
- `max_queue_depth`
- `default_mailbox_size`
- `enable_network`
- `tcp_port`
- `spawn_timeout_ms`
- `enable_http_gateway`
- `http_bind_host`
- `http_port`
- `http_max_connections`
- `http_max_request_size`
- `http_reply_timeout_ms`
- `use_coroutines`
- `imports`

Use `TomlTableView::read_string`, `read_uint32`, `read_bool`, and `for_each_string_array`.

### Step 4.2: Metrics parser

Add `src/config/parsers/metrics_config_parser.cpp`.

Parser name: `system.metrics`.

Order: `100`.

Responsibilities:

- Read `[system.metrics]`.
- Preserve defaults when the table is absent.
- Parse `enabled`, `ring_buffer_capacity`, and `metrics_path`.

### Step 4.3: Logging parser

Add `src/config/parsers/logging_config_parser.cpp`.

Parser name: `system.logging`.

Order: `110`.

Responsibilities:

- Read `[system.logging]`.
- Parse `enabled`, `default_level`, `format`, `ring_buffer_capacity`, `flush_on_level`, `file_path`, `drop_policy`, and `sinks`.
- Parse `[system.logging.levels]`.
- Parse `[system.logging.rotating_file]`.
- Keep existing behavior for unrecognized log levels, categories, and sinks by ignoring them.

### Step 4.4: CLI parser

Add `src/config/parsers/cli_config_parser.cpp`.

Parser name: `system.cli`.

Order: `120`.

Responsibilities:

- Read `[system.cli]`.
- Parse `enabled`, `listen_path`, `tcp_port`, `default_format`, and `page_size`.

### Step 4.5: Discovery parser

Add `src/config/parsers/discovery_config_parser.cpp`.

Parser name: `system.discovery`.

Order: `130`.

Responsibilities:

- Read `[system.discovery]`.
- Parse `backend` into `SystemDef::discovery_backend`.
- Leave nested gossip and static discovery keys untouched until `SystemDef` grows typed fields for them.

### Step 4.6: Topology parser

Add `src/config/parsers/topology_config_parser.cpp`.

Parser name: `topology.document`.

Order: `0`.

Responsibilities:

- Parse `[[dispatcher]]` into `TomlFileData::dispatchers`.
- Parse `[template.*]` into `TomlFileData::templates`.
- Parse `[[actor]]` into `TomlFileData::actors`.
- Own actor helper parsing:
  - `parse_dispatch_policy`
  - `parse_dispatcher`
  - `parse_actor`
  - `parse_raw_actor`
  - resource parsing
  - args conversion for string, integer, floating point, and boolean TOML values.

### Step 4.7: Add static registration to each parser file

At the bottom of each parser source file, add the registrar object that matches the parser category.

For system parsers:

```cpp
namespace {

const TomlSystemParserRegistration<SystemCoreConfigParser>
    kRegisterSystemCoreConfigParser;

} // namespace
```

For the document parser:

```cpp
namespace {

const TomlDocumentParserRegistration<TopologyConfigParser>
    kRegisterTopologyConfigParser;

} // namespace
```

Each parser class must define:

```cpp
static constexpr std::string_view kName = "system.metrics";
static constexpr int kOrder = 100;
```

Use these order values:

1. `topology.document`: `0`
2. `system.core`: `0`
3. `system.metrics`: `100`
4. `system.logging`: `110`
5. `system.cli`: `120`
6. `system.discovery`: `130`

The registry sorts by parser `order()` then `name()`, so cross-translation-unit static initialization order does not control parse order.

## Phase 5: Shrink `toml_parser.cpp`

### Step 5.1: Replace private structs

Remove private `RawActor` and `FileData` from `src/config/toml_parser.cpp`.

Use:

- `TomlRawActor`
- `TomlFileData`

Update `resolve_templates()` and merge code accordingly.

### Step 5.2: Use registered parser factories

Do not add a built-in parser bootstrap function. `TomlParser` should trust the registry populated by static registrar objects in linked parser translation units.

At the beginning of `parse_file_data`, create parser snapshots:

```cpp
auto document_parsers =
    TomlParserRegistry::instance().create_document_parsers();
auto system_parsers =
    TomlParserRegistry::instance().create_system_parsers();
```

If the document parser list is empty, or the system parser list is empty for an entrypoint file, return `errors::unknown` and log a config error. This catches build wiring mistakes where a parser source was not linked into `hpactor_lib`.

### Step 5.3: Rewrite `parse_file_data`

The new `parse_file_data` should:

1. Parse the file into a `toml::table`.
2. Wrap it in `TomlTableView`.
3. Reject imported files containing `[system]`.
4. For entrypoint files, require `[system]`.
5. Run all document parsers.
6. Run all system parsers for entrypoint files.
7. Return `TomlFileData`.

The function should not contain subsystem field-level parsing after this phase.

Expected shape:

```cpp
static result<TomlFileData>
parse_file_data(const std::string& filepath, bool is_entrypoint) {
    TomlFileData data;

    toml::table root;
    try {
        root = toml::parse_file(filepath);
    } catch (const toml::parse_error&) {
        error err(errors::unknown);
        HPACTOR_LOG_ERROR(log::LogCategory::kConfig, ActorId{0}, 0,
                          "topology parse error");
        return result<TomlFileData>::make(std::move(err));
    }

    TomlParseContext ctx(filepath, is_entrypoint);
    TomlTableView root_view = make_toml_table_view(&root);

    if (!is_entrypoint && root_view.contains("system")) {
        return result<TomlFileData>::make(error(errors::unknown));
    }

    if (is_entrypoint) {
        TomlTableView system_view = root_view.table("system");
        if (!system_view.valid()) {
            return result<TomlFileData>::make(error(errors::unknown));
        }
    }

    auto document_parsers =
        TomlParserRegistry::instance().create_document_parsers();
    auto system_parsers =
        TomlParserRegistry::instance().create_system_parsers();

    if (document_parsers.empty() ||
        (is_entrypoint && system_parsers.empty())) {
        return result<TomlFileData>::make(error(errors::unknown));
    }

    for (const auto& parser : document_parsers) {
        auto parsed = parser->parse(root_view, data, ctx);
        if (!parsed.has_value())
            return result<TomlFileData>::make(parsed.error());
    }

    if (is_entrypoint) {
        TomlTableView system_view = root_view.table("system");
        for (const auto& parser : system_parsers) {
            auto parsed = parser->parse(system_view, data.system, ctx);
            if (!parsed.has_value())
                return result<TomlFileData>::make(parsed.error());
        }
    }

    return result<TomlFileData>::make(std::move(data));
}
```

Adjust the exact logging fields to match the existing logger helper signatures.

### Step 5.4: Preserve parse pipeline

Leave this code behavior unchanged:

- `expand_glob`
- imported data collection
- imported-first merge ordering
- first-wins template merge behavior
- `deep_merge`
- `resolve_templates`
- `validate`
- `topological_sort`
- final `TopologyModel` assembly

## Phase 6: Registry Tests

### Step 6.1: Add `test_toml_parser_registry`

Create `tests/config/test_toml_parser_registry.cpp`.

Test cases:

- A custom system parser can be registered by a file-scope static registrar object.
- The custom parser leaves the model unchanged when its section is absent.
- Duplicate parser names are rejected.
- Built-in parsers still run when a custom parser is registered.

Custom parser behavior:

- Name: `system.test_custom`.
- Order: `1000`.
- Reads `[system.test_custom]`.
- If `version_override` exists, writes it to `SystemDef::version`.

Registration snippet:

```cpp
namespace {

class CustomVersionParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.test_custom";
    static constexpr int kOrder = 1000;

    std::string_view name() const noexcept override { return kName; }
    int order() const noexcept override { return kOrder; }

    result<void> parse(const TomlTableView& system,
                       SystemDef& out,
                       TomlParseContext&) const override {
        auto custom = system.table("test_custom");
        if (!custom.valid())
            return result<void>::make();
        out.version = custom.read_string("version_override", out.version);
        return result<void>::make();
    }
};

class DuplicateCustomVersionParser final : public ITomlSystemConfigParser {
  public:
    static constexpr std::string_view kName = "system.test_custom";
    static constexpr int kOrder = 1001;

    std::string_view name() const noexcept override { return kName; }
    int order() const noexcept override { return kOrder; }

    result<void> parse(const TomlTableView&,
                       SystemDef&,
                       TomlParseContext&) const override {
        return result<void>::make();
    }
};

const TomlSystemParserRegistration<CustomVersionParser>
    kRegisterCustomVersionParser;
const TomlSystemParserRegistration<DuplicateCustomVersionParser>
    kRegisterDuplicateCustomVersionParser;

} // namespace
```

Inline TOML fixture:

```toml
[system]
version = "1.0"

[system.test_custom]
version_override = "custom-version"

[[actor]]
id = "echo"
behavior = "EchoActor"
```

Assert:

- `kRegisterCustomVersionParser.registered()` returns `true`.
- `kRegisterDuplicateCustomVersionParser.registered()` returns `false`.
- `TomlParser::parse()` returns success.
- `model.system.version == "custom-version"`.
- `model.actors[0].id == "echo"`.

### Step 6.2: Wire the test target

Modify `tests/CMakeLists.txt`:

```cmake
add_executable(test_toml_parser_registry config/test_toml_parser_registry.cpp)
target_link_libraries(test_toml_parser_registry hpactor)
add_test(NAME test_toml_parser_registry COMMAND test_toml_parser_registry)
```

## Phase 7: Build Wiring

### Step 7.1: Add new sources

Modify the `hpactor_lib` source list in `CMakeLists.txt`:

```cmake
src/config/toml_table_view.cpp
src/config/toml_parser_registry.cpp
src/config/parsers/system_core_config_parser.cpp
src/config/parsers/metrics_config_parser.cpp
src/config/parsers/logging_config_parser.cpp
src/config/parsers/cli_config_parser.cpp
src/config/parsers/discovery_config_parser.cpp
src/config/parsers/topology_config_parser.cpp
```

### Step 7.2: Keep `toml.hpp` out of parser interfaces

Run:

```bash
rg -n "#include <toml.hpp>|#include \"toml.hpp\"" include src/config
```

Expected matches after the refactor:

- `src/config/toml_parser.cpp`
- `src/config/toml_table_view.cpp`

If any built-in parser source includes `toml.hpp`, move that logic behind `TomlTableView` instead.

## Phase 8: Documentation Updates

### Step 8.1: Update architecture docs

Modify `docs/architecture/core/actor-toml-config-architecture.md`:

- Add a section named `Extensible Parser Registry`.
- Document the parser categories.
- Show how future subsystems self-register parsers with static registrar objects without editing `parse_file_data`.
- Mention that imports and template resolution remain centralized.

Example documentation snippet:

```md
### Extensible Parser Registry

`TomlParser` owns document loading and topology assembly. Subsystems own their
own section parsers and self-register them through file-scope static registrar
objects.

System parsers receive the entrypoint `[system]` table and mutate `SystemDef`.
Document parsers receive each TOML document root and append dispatchers,
templates, or actors to `TomlFileData`.
```

### Step 8.2: Update repo memory and guidance

Modify:

- `AGENTS.md`
- `CLAUDE.md`
- `CLAUDE_MEMORY.md`

Record that `TomlParser` now uses an IoC parser registry and that new TOML subsystem config must be added through a static self-registered parser rather than direct edits to `parse_file_data`.

## Phase 9: Verification

Run targeted tests:

```bash
cmake -S . -B build -GNinja
ninja -C build test_toml_parser test_toml_parser_registry test_binary_roundtrip
./build/tests/test_toml_parser
./build/tests/test_toml_parser_registry
./build/tests/test_binary_roundtrip
```

Run CTest for the config area:

```bash
ctest --output-on-failure -R "toml|binary_roundtrip|bootstrap_engine|actor_factory_registry"
```

Run a full build if targeted tests pass:

```bash
ninja -C build
ctest --output-on-failure
```

Run source checks:

```bash
rg -n "parse_file_data|system.metrics|system.logging|system.cli|system.discovery" src/config include/hpactor/config tests/config
rg -n "#include <toml.hpp>|#include \"toml.hpp\"" include src/config
```

Expected source check outcomes:

- `parse_file_data` remains in `src/config/toml_parser.cpp` but only coordinates parser invocation.
- Subsystem strings appear in their parser source files and tests.
- `toml.hpp` appears only in `src/config/toml_parser.cpp` and `src/config/toml_table_view.cpp`.

## Acceptance Criteria

- `TomlParser::parse(const std::string&)` public behavior remains compatible.
- `parse_file_data` no longer contains field-level parsing for metrics, logging, CLI, discovery, actors, templates, or dispatchers.
- Each existing config subsystem has an isolated parser source file.
- New subsystem parsers self-register through file-scope static registrar objects without editing `parse_file_data`.
- Built-in parser registration uses static self-registration and deterministic registry sorting, so parse order is not dependent on static initialization order.
- No public HPActor header includes `toml.hpp`.
- Only TOML adapter sources require `-fexceptions`.
- Existing TOML parser tests, binary roundtrip tests, and bootstrap tests pass.
- New registry tests prove static custom parser registration and duplicate parser rejection.

## Implementation Notes

- Keep unknown `[system.*]` tables ignored by default to preserve current permissive behavior.
- Do not change `TopologyModel` field names for this refactor.
- Do not change binary topology format in this refactor.
- Do not move validation into subsystem parsers unless a subsystem needs section-local validation.
- Keep parser classes small and final.
- Prefer descriptive parser names such as `system.logging` because duplicate-name rejection is the primary guard against accidental double registration.
- Use registry snapshots while parsing so a parser cannot observe partial mutation of the registry list.

## Future Follow-Up

After this refactor lands, new TOML features should be added as small self-registering parser translation units:

- Mailbox management parser: `system.mailbox` plus actor mailbox table parsing.
- Distributed tracing parser: `system.tracing`.
- Service discovery typed parser expansion: nested gossip and static node settings.

Those follow-up features should not edit `parse_file_data`.
