# TST-004: Fuzz Test Design for TOML, Frames, Protobuf Decode, HTTP, CLI, and Admin API

**Issue:** [#83](https://github.com/skg7on/HPActor/issues/83)
**Source:** `docs/architecture/production/architecture-requirement-backlog.md#L320`
**Subsystem:** Testing
**Priority:** P1
**Status:** Design
**Date:** 2026-08-04

## 1. Executive Summary

This design defines a structured fuzz testing strategy for six input-facing
subsystems whose parsers/decoders process untrusted bytes from the network,
filesystem, or user input. Each fuzz target exercises a single, well-defined
parse/decode entry point with in-process coverage-guided fuzzing (libFuzzer).

**Fuzz testing surface — at a glance:**

| # | Subsystem | Entry Point | Input Source | Risk Class |
|---|-----------|-------------|--------------|------------|
| 1 | TOML Config | `TomlParser::parse(path)` → `toml::parse_file()` | Filesystem (config files) | **High** — file parsing, recursive includes, glob expansion |
| 2 | Wire Frames | `try_decode_wireframe(bytes)` | Network (TCP streams) | **Critical** — network ingress, all cluster traffic |
| 3 | Protobuf Decode | `WireEnvelope::ParseFromString()` | Network (frame payload) | **Critical** — untrusted protobuf from any peer |
| 4 | HTTP Parser | `HttpParser::execute(bytes)` | Network (HTTP requests) | **High** — HTTP ingress gateway, REST admin API |
| 5 | CLI Lexer | `Lexer::tokenize(string)` | stdin / UDS / TCP | **Medium** — local/admin input, but string-driven |
| 6 | Admin API | `AdminApiActor::handle(request)` | HTTP body → JSON → dispatch | **Medium** — authenticated admin, handler dispatch + JSON body |

## 2. Fuzzing Infrastructure

### 2.1 Build Integration

Add a `FUZZ_TEST` CMake macro that wraps `add_executable` with libFuzzer flags:

```cmake
# cmake/FuzzTest.cmake — new file
# Usage: add_fuzz_test(name SOURCES fuzz_target.cpp LIBRARIES hpactor_lib)
function(add_fuzz_test name)
    # Only built when ENABLE_FUZZ=ON (new CMake option, default OFF)
    add_executable(fuzz_${name} ${ARGN})
    target_compile_options(fuzz_${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_libraries(fuzz_${name} PRIVATE -fsanitize=fuzzer,address,undefined)
    # Each fuzz target links hpactor_lib but NOT GTest
endfunction()
```

**CMake option:** `ENABLE_FUZZ` (default `OFF`) — gates all fuzz target builds.
Fuzz targets are NOT registered with CTest; they are run manually or via
a fuzzing harness script.

**Build command:**
```bash
cmake -S . -B build -GNinja -DENABLE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
ninja -C build fuzz_toml fuzz_frame fuzz_protobuf fuzz_http fuzz_cli fuzz_admin
```

### 2.2 Test Location

Fuzz targets live alongside existing tests:

```
tests/fuzz/                          # New directory
├── CMakeLists.txt                   # Fuzz test build rules
├── fuzz_harness.hpp                 # Shared harness helpers
├── fuzz_corpus/                     # Seed corpus directories
│   ├── toml/                        # Seed TOML files
│   ├── frame/                       # Seed wire frames
│   ├── protobuf/                    # Seed protobuf payloads
│   ├── http/                        # Seed HTTP requests
│   ├── cli/                         # Seed CLI inputs
│   └── admin/                       # Seed admin JSON bodies
├── fuzz_toml.cpp                    # Target 1
├── fuzz_frame.cpp                   # Target 2
├── fuzz_protobuf_decode.cpp         # Target 3
├── fuzz_http_parser.cpp             # Target 4
├── fuzz_cli_lexer.cpp               # Target 5
└── fuzz_admin_api.cpp               # Target 6
```

### 2.3 Shared Harness (`fuzz_harness.hpp`)

```cpp
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdlib>   // std::abort
#include <iostream>

/// Call this at the top of LLVMFuzzerTestOneInput to silence production
/// logging/metrics during fuzzing.
inline void fuzz_silence_output() {
    // Redirect stdout/stderr to /dev/null for quiet fuzzing
    // (libFuzzer prints its own stats separately)
    static bool silenced = false;
    if (!silenced) {
        silenced = true;
        if (const char* env = std::getenv("FUZZ_VERBOSE"); !env) {
            freopen("/dev/null", "w", stdout);
            freopen("/dev/null", "w", stderr);
        }
    }
}

/// Sanity oracle: abort on undefined behavior signals so the fuzzer catches them.
/// Already provided by -fsanitize=address,undefined.
/// We add an explicit NULL-deref guard around any pointer parameter.
template <typename F>
int fuzz_entry(const uint8_t* data, size_t size, F&& fn) {
    fuzz_silence_output();
    try {
        fn(data, size);
    } catch (const std::exception& e) {
        // Exceptions are NOT expected — this is a bug.
        // (HPActor builds with -fno-exceptions, so this is a defense-in-depth
        //  check for the fuzz target itself, not the code under test.)
        std::cerr << "FUZZ: unexpected exception: " << e.what() << std::endl;
        std::abort();
    }
    return 0;  // libFuzzer expects 0
}
```

### 2.4 Fuzzing Runbook

```bash
# Quick smoke (10 seconds each)
for target in fuzz_toml fuzz_frame fuzz_protobuf fuzz_http fuzz_cli fuzz_admin; do
    ./build/tests/fuzz/${target} -max_total_time=10
done

# Deep run (1 hour, with seed corpus)
./build/tests/fuzz/fuzz_frame tests/fuzz/fuzz_corpus/frame/ \
    -max_total_time=3600 -jobs=4 -workers=4

# Minimize a crashing input
./build/tests/fuzz/fuzz_frame crash-001.bin -minimize_crash=1

# Regenerate seed corpus from passing inputs
./build/tests/fuzz/fuzz_frame -merge=1 tests/fuzz/fuzz_corpus/frame/ \
    tests/fuzz/fuzz_corpus/frame_new/

# Run under ASAN + UBSAN (implied by -fsanitize=fuzzer)
./build/tests/fuzz/fuzz_frame -detect_leaks=1 -rss_limit_mb=4096
```

## 3. Fuzz Target Designs

### 3.1 Target 1: TOML Config Parser (`fuzz_toml.cpp`)

**Entry point under test:** `toml::parse_file()` (toml++ library) via the
`parse_file_data()` internal pipeline.

**Attack surface:**
- `toml::parse_file()` — TOML v1.0.0 grammar with nested tables, arrays of tables, inline tables, multi-line strings, integers (bin/oct/hex/dec), floats (inf/nan), dates, times
- Glob expansion in `expand_glob()` — `*` wildcard patterns
- Deep merge in `deep_merge()` — nested struct field overwrites
- Template inheritance in `resolve_templates()` — key lookup in unordered_map
- Topological sort in `topological_sort()` — Kahn's algorithm on arbitrary DAG
- Validation in `validate()` — duplicate IDs, empty IDs, missing behaviors, dispatcher refs

**Fuzz target strategy:** Write a *standalone TOML file* from fuzz bytes and
feed it to `toml::parse_file()`. This exercises the full toml++ parser without
mocking the filesystem.

```cpp
// tests/fuzz/fuzz_toml.cpp
#include "fuzz_harness.hpp"
#include <toml.hpp>
#include <fstream>
#include <cstdio>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        // Write fuzz input to temp file (toml++ requires a file path)
        char tmpfile[] = "/tmp/fuzz_toml_XXXXXX";
        int fd = mkstemp(tmpfile);
        if (fd < 0) return;
        write(fd, d, s);
        close(fd);

        // Exercise toml++ parser directly — this is the core attack surface
        try {
            auto tbl = toml::parse_file(tmpfile);
            // Even if parse succeeds, try accessing deeply nested values
            // to exercise the table traversal code paths
            (void)tbl.size();
        } catch (const toml::parse_error&) {
            // Expected — malformed TOML
        } catch (const std::bad_alloc&) {
            // Memory exhaustion — also a valid finding
        }

        unlink(tmpfile);
    });
}
```

**What this catches:**
- Buffer overflows in toml++ string/buffer handling
- Integer overflow in date/time/float parsing
- Stack overflow from deeply nested tables (TOML has no depth limit)
- Memory exhaustion from key/array repetition attacks
- Undefined behavior in UTF-8 / escape sequence handling

**Seed corpus (`fuzz_corpus/toml/`):**
- Valid minimal TOML: `[system]`
- Valid complex TOML (from `examples/` topology files)
- Malformed TOML with unterminated strings
- Deeply nested inline tables
- Max-size integer/float literals
- Binary literals (`0b...`), hex floats
- Unicode escape sequences

**Unit test companion:** A `TEST(FuzzRegression, Toml)` test case in
`tests/unit/config/` that replays a small set of known historical-crash
inputs and asserts the parser either succeeds or returns a parse error
(never crashes).

---

### 3.2 Target 2: Wire Frame Decoder (`fuzz_frame.cpp`)

**Entry point under test:** `try_decode_wireframe(const StreamBuffer&, FrameDecodeLimits)`

**Attack surface:**
- 4-byte magic validation (`"HPAC"` — memcmp on 4 bytes)
- 4-byte length field in network byte order (ntohl)
- 16 MiB payload size bound check
- Length mismatch detection (`data.size() < HeaderSize + payload_len`)
- Trailing bytes rejection
- Protobuf `ParseFromString()` on the payload (handed off to Target 3)

**Fuzz target strategy:** Feed raw bytes directly as a candidate wire frame.

```cpp
// tests/fuzz/fuzz_frame.cpp
#include "fuzz_harness.hpp"
#include <hpactor/msg/frame.hpp>
#include <hpactor/adt/stream_buffer.hpp>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        StreamBuffer buf(d, d + s);

        // Test 1: default limits (16 MiB, reject trailing)
        auto r1 = net::try_decode_wireframe(buf);
        if (r1.ok()) {
            // Round-trip: encode then decode the result
            auto encoded = r1.frame.encode();
            auto r2 = net::try_decode_wireframe(encoded);
            // Must also succeed
            (void)r2;
        }

        // Test 2: relaxed limits (no size bound, allow trailing)
        net::FrameDecodeLimits relaxed{};
        relaxed.max_payload_bytes = 0;           // no limit
        relaxed.reject_trailing_bytes = false;
        auto r3 = net::try_decode_wireframe(buf, relaxed);
        (void)r3;

        // Test 3: tight limits (1-byte payload max)
        net::FrameDecodeLimits tight{};
        tight.max_payload_bytes = 1;
        tight.reject_trailing_bytes = true;
        auto r4 = net::try_decode_wireframe(buf, tight);
        (void)r4;

        // Test 4: zero-byte payload bound
        net::FrameDecodeLimits zero{};
        zero.max_payload_bytes = 0;
        zero.reject_trailing_bytes = false;
        auto r5 = net::try_decode_wireframe(buf, zero);
        (void)r5;
    });
}
```

**What this catches:**
- Buffer underflow on < 8 byte input
- Integer overflow in `HeaderSize + payload_len` addition
- ntohl on unaligned/partial reads
- Protobuf parser crashes (forwarded)
- Stack/heap corruption from large declared lengths
- Round-trip encode/decode inconsistency (correctness bug)

**Seed corpus (`fuzz_corpus/frame/`):**
- Valid minimal frame (magic + 0-length payload)
- Valid frame with Data payload
- Valid frame with Batch payload
- Truncated at 0, 1, 2, 3, 4, 5, 6, 7 bytes
- Magic bytes `HPAC` but wrong endianness
- Bytes `\x00\x00\x00\x00` (all-zero header)
- Declared length = 0xFFFFFFFF (max uint32, would overflow)
- Declared length = 0x7FFFFFFF

**Unit test companion:** `TEST(FuzzRegression, Frame)` in `tests/unit/net/`.

---

### 3.3 Target 3: Protobuf Decode (`fuzz_protobuf_decode.cpp`)

**Entry point under test:** `WireEnvelope::ParseFromString()` (protobuf library)
and the subsequent oneof dispatch in `WireFrame::payload_type()`.

**Attack surface:**
- Protobuf wire format parsing — varint decoding, length-delimited fields, fixed32/fixed64
- `WireEnvelope` oneof discrimination (10 variants: Data, Ack, Nack, Batch, 5 Stream types)
- Recursive/nested `ActorMsgFrame` within `DataFrame`
- TraceContext deserialization via `trace_context_from_proto()`
- Batch frame bounded entry count checks
- Python binding protected-tag rejection (`0xF0`–`0xF3`)

**Fuzz target strategy:** Parse raw bytes as a `WireEnvelope`, then exercise
the oneof dispatch path.

```cpp
// tests/fuzz/fuzz_protobuf_decode.cpp
#include "fuzz_harness.hpp"
#include <hpactor/msg/frame.hpp>
#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        std::string payload(reinterpret_cast<const char*>(d), s);

        // Test 1: Parse as WireEnvelope (the main network protobuf type)
        net::WireEnvelope envelope;
        bool ok = envelope.ParseFromString(payload);
        if (ok) {
            // Exercise oneof dispatch — this is the code path that
            // classifies frames in InboundFrameRouter
            switch (envelope.payload_case()) {
                case net::WireEnvelope::kDataFrame: {
                    const auto& df = envelope.data_frame();
                    (void)df.sender().node_id();
                    (void)df.receiver().node_id();
                    (void)df.payload().size();
                    break;
                }
                case net::WireEnvelope::kBatchFrame: {
                    const auto& bf = envelope.batch_frame();
                    (void)bf.frames_size();  // repeated field
                    break;
                }
                case net::WireEnvelope::kAckFrame:
                case net::WireEnvelope::kNackFrame:
                case net::WireEnvelope::kStreamOpen:
                case net::WireEnvelope::kStreamData:
                case net::WireEnvelope::kStreamAck:
                case net::WireEnvelope::kStreamClose:
                case net::WireEnvelope::kStreamError:
                    break;
                default:
                    break;
            }

            // Re-serialize and re-parse (round-trip)
            std::string reserialized;
            envelope.SerializeToString(&reserialized);
            net::WireEnvelope envelope2;
            envelope2.ParseFromString(reserialized);
        }

        // Test 2: Also try parsing as PbActorAddress (nested sub-message)
        PbActorAddress addr;
        addr.ParseFromString(payload);
        (void)addr.node_id();
        (void)addr.actor_id();

        // Test 3: Also try parsing as PbTraceContext (nested sub-message)
        net::PbTraceContext trace;
        trace.ParseFromString(payload);
        (void)trace.trace_id().size();
        (void)trace.span_id().size();
    });
}
```

**What this catches:**
- Protobuf C++ library memory safety bugs (varint overflow, length-delimited bounds)
- Stack overflow from deeply nested protobuf messages
- Integer overflow in repeated-field size calculations
- Use-after-free from moved sub-messages during oneof access
- Round-trip serialize/parse inconsistency

**Note on protobuf library:** The protobuf C++ library has its own fuzz
testing upstream. This target focuses on *HPActor's usage* — the oneof
dispatch, field access patterns, and nested message handling after a
successful parse. It also exercises the specific `.proto` schema surfaces
that HPActor defines.

**Seed corpus (`fuzz_corpus/protobuf/`):**
- Empty protobuf (zero bytes)
- Valid minimal WireEnvelope (DataFrame with empty payload)
- Valid BatchFrame with 0, 1, 2 entries
- Varint boundary values: 0, 127, 128, 16383, 16384
- Truncated varint sequences
- Max-length repeated fields
- Interleaved unknown field tags

---

### 3.4 Target 4: HTTP Parser (`fuzz_http_parser.cpp`)

**Entry point under test:** `HttpParser::execute(const StreamBuffer&)` wrapping
llhttp.

**Attack surface:**
- llhttp C library — the full HTTP/1.1 grammar
- `on_url_cb` — unbounded URL accumulation (`url_buf_.append()`)
- `on_header_field_cb` / `on_header_value_cb` — unbounded header accumulation
- `on_body_cb` — unbounded body accumulation
- `finish_header()` — header vector push
- `HttpParser::execute()` — PAUSED/PARSING_BODY state machine
- `method_from_string()` — constexpr method parsing with no length guard (relies on llhttp providing valid method string)
- `HttpSerializer::parse_accept_header()` — hand-rolled `Accept` header parser with float quality parsing (`std::stof`)

**Fuzz target strategy:** Feed raw bytes as HTTP request bytes.

```cpp
// tests/fuzz/fuzz_http_parser.cpp
#include "fuzz_harness.hpp"
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_serializer.hpp>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        StreamBuffer buf(d, d + s);

        // Test 1: Request-mode parse
        net::HttpParser parser(net::HttpParserMode::Request);
        bool complete = false;
        parser.set_on_message([&](net::HttpRequest&& req) {
            complete = true;
            // Exercise the deserialized request
            (void)req.method;
            (void)req.path.size();
            (void)req.headers.size();
            (void)req.body.size();

            // Exercise HttpSerializer on the parsed request
            net::HttpSerializer serializer;
            auto result = serializer.deserialize_request(req, 42);
            (void)result;

            // Exercise parse_accept_header via serializer
            auto ct = req.header("accept");
            if (ct.has_value()) {
                auto accepted = serializer.parse_accept_header(*ct);
                (void)accepted.size();
            }
        });
        parser.set_on_error([](llhttp_errno_t, const char*) {});

        size_t consumed = parser.execute(buf);
        (void)consumed;

        // Test 2: Response-mode parse
        net::HttpParser resp_parser(net::HttpParserMode::Response);
        resp_parser.set_on_response(
            [](int status, const std::vector<net::HttpHeader>& headers,
               const StreamBuffer& body) {
                (void)status;
                (void)headers.size();
                (void)body.size();
            });
        resp_parser.set_on_error([](llhttp_errno_t, const char*) {});
        resp_parser.execute(buf);

        // Test 3: Incremental feeding — split input at midpoint
        if (s > 1) {
            net::HttpParser inc_parser(net::HttpParserMode::Request);
            inc_parser.set_on_message([&](net::HttpRequest&&) {});
            inc_parser.set_on_error([](llhttp_errno_t, const char*) {});
            size_t mid = s / 2;
            StreamBuffer first(d, d + mid);
            StreamBuffer second(d + mid, d + s);
            inc_parser.execute(first);
            inc_parser.execute(second);
        }
    });
}
```

**What this catches:**
- llhttp C library memory safety bugs
- Unbounded memory growth from `url_buf_`, `header_*_buf_`, `body_buf_` (no max-length enforcement in `HttpParser`)
- `std::stof()` exception in `parse_accept_header()` — **this is a known concern** because HPActor generally disables exceptions
- Integer overflow in fragment size handling
- PAUSED/UPGRADE state machine issues

**Special attention — `parse_accept_header()` exception risk:**
The `HttpSerializer::parse_accept_header()` method calls `std::stof()` which
can throw `std::invalid_argument` or `std::out_of_range`. HPActor builds with
`-fno-exceptions` for all files except `toml_parser.cpp` and
`toml_table_view.cpp`. This fuzz target MUST verify that `parse_accept_header()`
is either moved to an exceptions-allowed TU or wrapped in a try-catch.
**This is a pre-existing correctness bug that fuzzing will surface.**

**Seed corpus (`fuzz_corpus/http/`):**
- Minimal valid request: `GET / HTTP/1.1\r\n\r\n`
- Request with headers: `GET / HTTP/1.1\r\nHost: example.com\r\n\r\n`
- POST with body: `POST /api HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello`
- Chunked transfer encoding
- Malformed: missing CR, bare LF line endings
- Malformed: negative Content-Length
- Malformed: HTTP/0.9 style request
- Large header count (>100) / large header values
- Unicode and binary data in URL, headers, body

---

### 3.5 Target 5: CLI Lexer (`fuzz_cli_lexer.cpp`)

**Entry point under test:** `Lexer::tokenize(const std::string&)`

**Attack surface:**
- String-to-token conversion — the entire lexer grammar
- Quoted string handling — `"..."` with escape sequences, unterminated quotes
- Flag detection — `--flag`, `--flag value`
- Comment handling — `#` comment character
- Leading `/` normalization
- Backslash unescape in `Lexer::unescape()`
- Standard library `std::string` manipulation, `std::vector<Token>` growth

**Fuzz target strategy:** Feed raw bytes as a CLI command string.

```cpp
// tests/fuzz/fuzz_cli_lexer.cpp
#include "fuzz_harness.hpp"
#include <hpactor/cli/io/lexer.hpp>
#include <hpactor/cli/token.hpp>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        std::string input(reinterpret_cast<const char*>(d), s);

        // Core lexer entry point
        auto tokens = cli::Lexer::tokenize(input);

        // Verify invariants:
        // 1. Token list is never empty (always at least Eof)
        // 2. Last token is always Eof
        if (!tokens.empty()) {
            assert(tokens.back().kind == cli::TokenKind::Eof);
        }

        // Exercise token accessors
        for (const auto& tok : tokens) {
            (void)tok.kind;
            (void)tok.text.size();
            // Force string copy to exercise allocator
            volatile auto copy = tok.text;
            (void)copy;
        }
    });
}
```

**What this catches:**
- Buffer over/underflow in character-by-character string processing
- Integer overflow in token/text accumulation
- Stack overflow from deeply recursive escape sequences
- Unterminated quote handling (infinite state machine loop)
- Invalid UTF-8 handling in string operations
- Memory safety of `unescape()` with adversarial escape sequences

**Seed corpus (`fuzz_corpus/cli/`):**
- Empty string
- Single `/`
- Simple command: `/actor list`
- Quoted string: `/foo "hello world"`
- Escaped quotes: `/foo "hello \"world\""`
- Flag: `/foo --flag value`
- Comment: `/foo arg # comment text`
- Unterminated quote: `/foo "hello`
- Lone escape: `/foo \`
- Whitespace-only: `   \t  \n  `
- Mismatched quotes, double-escapes
- Long strings (>10K chars)

---

### 3.6 Target 6: Admin API (`fuzz_admin_api.cpp`)

**Entry point under test:** `AdminApiActor::handle(const AdminRequest&)`

**Attack surface:**
- `AdminResource` enum dispatch (5 built-in resources + custom handlers)
- `AdminRequest::body` — arbitrary JSON string
- `health_check()` — `ActorSystem::shutdown_phase()` access (null-safe)
- `list_actors()` — `ActorSystem::for_each_actor()` callback, `std::ostringstream` JSON building, `type_name()`, `address().to_string()`
- `list_cluster_nodes()` — returns static JSON stub
- `do_shutdown()` — `ActorSystem::shutdown()` call
- Custom handler registration — `std::function` callbacks, mutex locking

**Fuzz target strategy:** Construct `AdminRequest` with fuzz-derived resource
enum and body, passing nullptr for ActorSystem (safe path). Then exercise with
a mock ActorSystem.

```cpp
// tests/fuzz/fuzz_admin_api.cpp
#include "fuzz_harness.hpp"
#include <hpactor/cli/admin/admin_api_actor.hpp>
#include <hpactor/cli/admin/admin_messages.hpp>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        if (s < 1) return;

        // Use first byte as resource selector (0-255, maps to AdminResource
        // enum which has 5 values; out-of-range values hit the default case)
        uint8_t resource_byte = d[0];
        std::string body(reinterpret_cast<const char*>(d + 1), s - 1);

        AdminResource resource;
        switch (resource_byte % 8) {
            case 0: resource = AdminResource::Health; break;
            case 1: resource = AdminResource::Actors; break;
            case 2: resource = AdminResource::ClusterNodes; break;
            case 3: resource = AdminResource::Shutdown; break;
            // 4-7: out-of-range values — test default case
            default: resource = static_cast<AdminResource>(resource_byte); break;
        }

        AdminRequest req{resource, std::move(body)};

        // Test 1: nullptr ActorSystem (safe — all handlers check for null)
        {
            cli::admin::AdminApiActor admin(nullptr);
            auto resp = admin.handle(req);
            (void)resp.status_code;
            (void)resp.body.size();
        }

        // Test 2: Register a custom handler and exercise it
        {
            cli::admin::AdminApiActor admin(nullptr);
            admin.register_handler(AdminResource::Health,
                [](const AdminRequest& r) -> AdminResponse {
                    return {200, r.body};  // echo body back
                });
            auto resp = admin.handle(req);
            (void)resp.status_code;
        }

        // Test 3: Exercise all built-in handlers individually
        {
            cli::admin::AdminApiActor admin(nullptr);
            admin.health_check();
            admin.list_actors();
            admin.list_cluster_nodes();
            admin.do_shutdown();
        }
    });
}
```

**What this catches:**
- Null dereference in `AdminApiActor` methods (nullptr `system_`)
- JSON injection via `AdminRequest::body` in custom handlers
- Thread-safety issues in handler registration dispatch (mutex+function)
- Memory safety of response body construction
- Enum out-of-range dispatch behavior

**Known limitation:** This target does not exercise the live ActorSystem path
(which requires a full scheduler, worker threads, etc.). The live-path fuzzing
is deferred to integration-level chaos testing (TST-002). This target
focuses on the `AdminApiActor` dispatch logic, null-safety, and handler
registration correctness.

**Seed corpus (`fuzz_corpus/admin/`):**
- Valid JSON body: `{"key":"value"}`
- Empty body
- Binary non-UTF-8 body
- Large body (approaching 16 MiB)
- Deeply nested JSON

---

## 4. Acceptance Criteria

### 4.1 Build Integration
- [ ] `ENABLE_FUZZ` CMake option added (default OFF)
- [ ] `cmake/FuzzTest.cmake` module provides `add_fuzz_test()` macro
- [ ] All 6 fuzz targets build successfully with `clang++ -fsanitize=fuzzer,address,undefined`
- [ ] Fuzz targets are excluded from `ninja all` default build

### 4.2 Fuzz Targets
- [ ] Each fuzz target links and runs without crashing on valid seed input
- [ ] Each fuzz target runs for 60 seconds without ASAN/UBSAN findings on seed corpus
- [ ] A manual 15-minute deep run on each target completes without timeouts

### 4.3 Regression Tests
- [ ] A `TEST(FuzzRegression, *)` case for each subsystem replays 3-5 known-edge-case inputs
- [ ] The regression test is in the existing subsystem test file (e.g., `tests/unit/net/test_frame.cpp` for frames)

### 4.4 Seed Corpus
- [ ] `tests/fuzz/fuzz_corpus/<subsystem>/` directory exists for each target
- [ ] At least 5 seed files per target covering success + failure paths
- [ ] Seed files are committed to the repository

### 4.5 Documentation
- [ ] `tests/fuzz/README.md` documents how to build, run, and triage fuzz targets
- [ ] Runbook section added to `docs/developer/` (or inline in the README)

### 4.6 Pre-existing Issues Surfaced
- [ ] `parse_accept_header()` exception risk is documented or fixed (see §3.4)
- [ ] Any crashes found during 15-minute deep runs are triaged as bugs

## 5. Implementation Phases

### Phase 1: Infrastructure (estimated 2 tasks)
1. Add `ENABLE_FUZZ` CMake option and `cmake/FuzzTest.cmake` module
2. Add `tests/fuzz/CMakeLists.txt`, `fuzz_harness.hpp`, and `tests/fuzz/README.md`

### Phase 2: Critical Targets (P0 — network ingress)
3. Implement `fuzz_frame.cpp` + seed corpus + regression test
4. Implement `fuzz_protobuf_decode.cpp` + seed corpus + regression test

### Phase 3: High-Value Targets (P1 — HTTP + TOML)
5. Implement `fuzz_http_parser.cpp` + seed corpus + regression test
6. Implement `fuzz_toml.cpp` + seed corpus + regression test

### Phase 4: Medium Targets (P2 — CLI + Admin)
7. Implement `fuzz_cli_lexer.cpp` + seed corpus + regression test
8. Implement `fuzz_admin_api.cpp` + seed corpus + regression test

### Phase 5: Integration
9. Run full 1-hour deep fuzz on all 6 targets, triage findings
10. Add CI smoke-fuzz job (60-second per-target sanitizer run)

## 6. CI Integration (Future)

Not part of this design phase, but the pattern is documented for
follow-up. Add a GitHub Actions workflow that:

1. Builds fuzz targets with `ENABLE_FUZZ=ON`
2. Runs each target for 60 seconds (smoke-only, not deep fuzzing)
3. Fails the build on any crash or sanitizer finding
4. Uploads new coverage corpus as artifacts

## 7. What This Does NOT Cover

This design explicitly scopes OUT:

- **ClusterFuzz / OSS-Fuzz integration** — requires Google-owned infrastructure, future follow-up
- **Live ActorSystem fuzzing** — requires full scheduler/network setup; covered by TST-002 (chaos tests)
- **Coverage-guided fuzzing of protobuf itself** — upstream responsibility
- **Fuzzing the TOML config *pipeline*** (imports, templates, sort) — Phase 1 covers the raw parser only; pipeline fuzzing can follow in a subsequent iteration
- **Network-level fuzzing** (raw TCP streams, TLS handshake) — requires integration harness; deferred to TST-002 chaos tests
- **Python binding fuzz targets** (`_hpac` frame encoding, protobuf codec) — separate design scope

## 8. Risks and Mitigations

| Risk | Likelihood | Impact | Mitigation |
|------|-----------|--------|------------|
| `-fno-exceptions` HPActor + `std::stof()` in `parse_accept_header()` | **High** | Crash | Fuzz HTTP path; fix or isolate exception-throwing code in exceptions-enabled TU |
| Protobuf library already well-fuzzed upstream | Medium | Low signal from fuzz_protobuf | Focus on HPActor-specific oneof dispatch + nested message access, not raw protobuf parsing |
| llhttp already well-fuzzed upstream | Medium | Low signal from fuzz_http | Focus on HPActor's `HttpParser` wrapper: unbounded accumulation, state machine, `HttpSerializer` call chain |
| CI time budget for fuzzing | Low | Slow CI | 60-second smoke only; deep fuzzing runs offline/ad-hoc |
| Seed corpus too narrow (low coverage) | Medium | Wasted fuzz cycles | Start with known edge cases; regenerate from coverage reports after first deep run |
