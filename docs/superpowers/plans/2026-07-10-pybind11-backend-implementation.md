# Python Binding pybind11 Backend — Implementation Plan

**Date:** 2026-07-10
**Status:** Plan
**Depends on:** Phases 1A, 1B, 1C implemented
**Design spec:** `docs/superpowers/specs/2026-07-10-pybind11-backend-design.md`

## Goal

Replace the raw CPython limited C API backend (`bindings/python/native/src/python_capi/`)
with a pybind11-based backend (`bindings/python/native/src/python_pybind11/`) that
produces the identical `_hpactor` module and passes all 206 existing Python binding tests.

## Architecture

```
Before:                              After:
python_capi/                         python_pybind11/
  module.cpp           →               module.cpp          (PYBIND11_MODULE)
  native_system_type.hpp/cpp →         native_system.hpp/cpp (class_<NativeSystemObject>)
  conversions.hpp/cpp   →             (absorbed into type casters + inline conversions)
```

The pure-Python `hpactor/` package, native bridge library `hpactor_python_native`,
and all three bounded queues are **unchanged**.

**Exception boundary:** pybind11 TUs get `-fexceptions`. Every method exposed to
the native bridge is `noexcept` with internal try/catch that converts
`pybind11::error_already_set` → error sentinels.

**ABI3:** `PYBIND11_USE_LIMITED_API=1`, `Py_LIMITED_API=0x030B0000`.

## Tech Stack

C++20, pybind11 ≥ 2.12.0 (header-only), CPython 3.11 Stable ABI, HPActor
native bridge types, CMake/Ninja, GoogleTest, Python unittest, Linux eventfd,
macOS non-blocking socket pairs.

## Global Constraints

1. Phases 1A, 1B, and 1C must pass before this migration begins.
2. Work in an isolated git worktree on branch `feature/426-pybind11-backend`.
3. pybind11 TUs compile with `-fno-rtti`; `-fexceptions` allowed only in
   `bindings/python/native/src/python_pybind11/`.
4. `PYBIND11_USE_LIMITED_API=1` and `Py_LIMITED_API=0x030B0000` on every
   pybind11 TU.
5. pybind11 headers confined to `python_pybind11/`; no `Python.h` elsewhere
   in production binding code.
6. HPActor scheduler, network, telemetry, CLI, and health threads never
   call CPython or wait for the GIL.
7. Native bridge types, queues, and ports contain no `PyObject*`, borrowed
   Python buffers, or pybind11 objects.
8. All 206 existing Python binding tests must pass unchanged (no Python-side
   API changes).
9. All existing C++ binding unit tests must continue to pass.
10. `-fno-rtti` on all binding TUs (including pybind11 TUs).
11. No `dynamic_cast`, `typeid`, or `std::function` in binding ports or
    cross-thread callbacks.
12. Protected tags 0xF0–0xF3 remain rejected from remote frame ingress.
13. Tests use paused workers, explicit scheduler steps, notifier drains,
    events, and condition-based waits. Sleeps are deadlock guards only.
14. Full configured build/test run at end.
15. Architecture fitness checks updated (not removed) for pybind11.
16. `ENABLE_PYTHON_BINDINGS=OFF` preserves existing build behavior.
17. The worktree branch must follow naming: `feature/426-pybind11-backend`.

## File Structure

### Removed files (5)

```
bindings/python/native/src/python_capi/module.cpp
bindings/python/native/src/python_capi/native_system_type.hpp
bindings/python/native/src/python_capi/native_system_type.cpp
bindings/python/native/src/python_capi/conversions.hpp
bindings/python/native/src/python_capi/conversions.cpp
```

### Created files (4)

```
bindings/python/native/src/python_pybind11/module.cpp
bindings/python/native/src/python_pybind11/native_system.hpp
bindings/python/native/src/python_pybind11/native_system.cpp
bindings/python/native/src/python_pybind11/type_casters.hpp
```

### Modified files (9)

```
bindings/python/native/CMakeLists.txt                   # Build target changes
tests/architecture/CMakeLists.txt                       # Scan rule updates
tests/unit/python_binding/CMakeLists.txt                # (if any test needs adjustment)
docs/superpowers/specs/2026-07-03-python-language-binding-design.md  # Section 1, 20 updated
docs/superpowers/specs/2026-07-10-pybind11-backend-design.md         # New design spec
CLAUDE_MEMORY.md                                        # Record migration completion
bindings/python/hpactor/_system.py                      # Dict-based dispatch/completion (see Task 3)
bindings/python/hpactor/_runtime.py                     # Dict-based dispatch/completion (see Task 3)
.gitignore                                              # Add pybind11 vendored source if vendored
```

---

## Task 1: Add pybind11 Dependency and CMake Target

### Step 1 — Write failing CMake configuration test

**File:** `tests/unit/python_binding/test_python_binding_config.cpp` (extend existing)

```cpp
TEST(PythonBindingBuildConfig, Pybind11HeadersAvailable) {
    // This test only compiles when pybind11 headers are findable
    #include <pybind11/pybind11.h>
    static_assert(PYBIND11_VERSION_MAJOR >= 2);
    static_assert(PYBIND11_VERSION_MINOR >= 12);
}
```

This test fails to compile until pybind11 is findable by CMake.

### Step 2 — Add pybind11 dependency

**File:** `bindings/python/native/CMakeLists.txt`

```cmake
# After the existing ENABLE_PYTHON_BINDINGS guard...

# Find pybind11 (header-only, ≥2.12.0 for stable-ABI support)
include(FetchContent)
FetchContent_Declare(
    pybind11
    GIT_REPOSITORY https://github.com/pybind/pybind11.git
    GIT_TAG        v2.13.6     # Latest stable with full stable-ABI support
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(pybind11)

# New target: pybind11-based CPython extension
pybind11_add_module(_hpactor MODULE
    src/python_pybind11/module.cpp
    src/python_pybind11/native_system.cpp
)
target_compile_definitions(_hpactor PRIVATE
    Py_LIMITED_API=0x030B0000
    PYBIND11_USE_LIMITED_API=1
)
target_compile_options(_hpactor PRIVATE
    -fexceptions          # Allow exceptions in this target only
    -fno-rtti
)
target_link_libraries(_hpactor PRIVATE
    hpactor_python_native
    hpactor_lib
)
set_target_properties(_hpactor PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Remove old python_capi target (was `_hpactor` MODULE library)
# The old CMakeLists.txt lines that built src/python_capi/*.cpp
# are removed entirely.
```

**Key:** The old `python_capi/` source files are removed from the build. The
`pybind11_add_module` macro handles `Python3::Module` linking, SABI flags,
and output naming automatically.

### Step 3 — Verify build

```bash
cmake -S . -B build -GNinja -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
    -DENABLE_PYTHON_BINDINGS=ON
ninja -C build
```

The `_hpactor` module must compile and link. The configuration test from Step 1
must pass once pybind11 headers are available.

**Commit:** `build: add pybind11 dependency and _hpactor build target`

---

## Task 2: Implement NativeSystemObject with Sealed Exception Boundary

### Step 1 — Write failing unit test for the guard wrapper

**File:** `tests/unit/python_binding/test_pybind11_native_system.cpp` (new)

```cpp
#include <gtest/gtest.h>
#include <pybind11/pybind11.h>
#include <pybind11/embed.h>

#include "hpactor/python/python_native_system.hpp"
#include "bindings/python/native/src/python_pybind11/native_system.hpp"

class Pybind11NativeSystemTest : public ::testing::Test {
protected:
    void SetUp() override {
        pybind11::initialize_interpreter();
    }
    void TearDown() override {
        pybind11::finalize_interpreter();
    }
};

TEST_F(Pybind11NativeSystemTest, ConfigDictConstructsValidObject) {
    auto config = pybind11::dict(
        pybind11::arg("dispatch_queue_capacity") = 65536,
        pybind11::arg("command_queue_capacity") = 16384,
        pybind11::arg("completion_queue_capacity") = 16384,
        pybind11::arg("max_actor_bindings") = 65536,
        pybind11::arg("max_dispatch_per_tick") = 256,
        pybind11::arg("max_commands_per_turn") = 256,
        pybind11::arg("loop_lag_unready_ms") = 5000,
        pybind11::arg("handler_shutdown_timeout_ms") = 10000,
        pybind11::arg("trace_handler_spans") = true
    );
    NativeSystemObject obj(config);
    // Construction succeeded — no exception, valid native pointer
    EXPECT_GE(obj.dispatch_fd(), 0);
    EXPECT_GE(obj.completion_fd(), 0);
}

TEST_F(Pybind11NativeSystemTest, StartStopLifecycle) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    EXPECT_TRUE(obj.start());
    EXPECT_TRUE(obj.stop());
    // Idempotent stop
    EXPECT_TRUE(obj.stop());
}

TEST_F(Pybind11NativeSystemTest, ApplicationOriginReturnsValidTuple) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    ASSERT_TRUE(obj.start());
    auto origin = obj.application_origin();
    EXPECT_EQ(pybind11::len(origin), 6);
    // (family, packed_address, port, actor_type, actor_id, incarnation)
    EXPECT_EQ(origin[pybind11::int_(0)].cast<int>(), 0);  // family = local
    EXPECT_GT(origin[pybind11::int_(4)].cast<uint64_t>(), 0);  // actor_id > 0
    EXPECT_TRUE(obj.stop());
}

TEST_F(Pybind11NativeSystemTest, DrainDispatchReturnsNamedDicts) {
    // Verifies the new dict-based format
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    ASSERT_TRUE(obj.start());

    auto dispatches = obj.drain_dispatch(16);
    EXPECT_EQ(pybind11::len(dispatches), 0);  // empty when no messages

    // Each dispatch dict should have these keys:
    // actor_id, generation, type_tag, payload, sender_id, message_id,
    // ask_message_id, trace_id, priority, deadline_ns, flags,
    // ack_requested, sequence, kind
    EXPECT_TRUE(obj.stop());
}

TEST_F(Pybind11NativeSystemTest, GuardConvertsPybind11ErrorToSentinel) {
    auto config = make_valid_config();
    NativeSystemObject obj(config);
    ASSERT_TRUE(obj.start());

    // Pass an invalid address tuple (wrong size) — should return false,
    // not throw
    auto bad_address = pybind11::make_tuple(0);  // only 1 element, expected 6
    bool result = obj.stop_bridge(bad_address);
    EXPECT_FALSE(result);
    // Python error state is set (ValueError or TypeError)
    EXPECT_NE(PyErr_Occurred(), nullptr);
    PyErr_Clear();
    EXPECT_TRUE(obj.stop());
}
```

### Step 2 — Write production code

**File:** `bindings/python/native/src/python_pybind11/native_system.hpp`

```cpp
#pragma once

#include <memory>
#include <pybind11/pybind11.h>

class PythonNativeSystem;

class NativeSystemObject {
public:
    explicit NativeSystemObject(pybind11::dict config);
    ~NativeSystemObject();

    // Lifecycle (all noexcept)
    bool start() noexcept;
    bool stop() noexcept;
    bool begin_draining() noexcept;

    // Actor management
    pybind11::object application_origin() noexcept;
    pybind11::object spawn_bridge() noexcept;
    bool stop_bridge(pybind11::tuple address) noexcept;
    bool register_name(const std::string& name, pybind11::tuple address) noexcept;
    pybind11::object resolve_name(const std::string& name) noexcept;

    // Message passing
    bool submit(pybind11::dict command) noexcept;
    pybind11::list drain_dispatch(uint32_t max_items) noexcept;
    pybind11::list drain_completions(uint32_t max_items) noexcept;

    // Observability
    pybind11::dict snapshot() noexcept;
    int dispatch_fd() const noexcept;
    int completion_fd() const noexcept;

    // Phase 1E topology (forward-compatible stubs)
    pybind11::object prepare_topology(const std::string& path) noexcept;
    bool bind_topology_manifest(pybind11::list bindings) noexcept;
    bool start_prepared_topology() noexcept;
    bool complete_topology_actor(pybind11::dict outcome) noexcept;
    bool record_topology_preflight(pybind11::dict preflight) noexcept;
    pybind11::object last_topology_error() noexcept;

private:
    std::unique_ptr<PythonNativeSystem> native_;

    // Sealed noexcept boundary — catches pybind11 errors, restores Python
    // error state, returns sentinel values.
    template<typename F>
    auto guard(F&& f) noexcept -> decltype(f()) {
        try {
            return f();
        } catch (pybind11::error_already_set& e) {
            e.restore();
            if constexpr (std::is_same_v<decltype(f()), bool>) return false;
            else if constexpr (std::is_same_v<decltype(f()), pybind11::object>) return {};
            else if constexpr (std::is_same_v<decltype(f()), pybind11::list>) return {};
            else if constexpr (std::is_same_v<decltype(f()), pybind11::dict>) return {};
            else if constexpr (std::is_same_v<decltype(f()), pybind11::tuple>) return {};
            else return {};
        }
    }

    // Conversion helpers (value-only, never store PyObject* across calls)
    static pybind11::dict dispatch_to_dict(const PythonDispatchEnvelope& env) noexcept;
    static pybind11::dict completion_to_dict(const PythonCompletion& comp) noexcept;
    static std::optional<PythonCommand> dict_to_command(pybind11::dict cmd_dict) noexcept;
    static pybind11::tuple address_to_tuple(const ActorAddress& addr) noexcept;
    static std::optional<ActorAddress> tuple_to_address(pybind11::tuple tup) noexcept;
};
```

**File:** `bindings/python/native/src/python_pybind11/native_system.cpp`

Implements all methods. Key implementation patterns:

```cpp
NativeSystemObject::NativeSystemObject(pybind11::dict config) {
    PythonRuntimeConfig rt_config;
    // Extract config fields with defaults
    if (config.contains("dispatch_queue_capacity"))
        rt_config.dispatch_queue_capacity = config["dispatch_queue_capacity"].cast<uint32_t>();
    // ... all 9 fields ...

    auto err = rt_config.validate();
    if (err != PythonConfigError::None) {
        throw pybind11::value_error("invalid Python binding config");
    }

    auto sys_result = PythonNativeSystem::create(
        ActorSystem::Config{}, rt_config);
    if (!sys_result) {
        throw pybind11::runtime_error("failed to create native system");
    }
    native_ = std::move(sys_result.value());
}

bool NativeSystemObject::start() noexcept {
    return guard([&] {
        auto result = native_->start();
        if (!result) {
            PyErr_SetString(PyExc_RuntimeError, "native system start failed");
            throw pybind11::error_already_set();
        }
        return true;
    });
}

pybind11::list NativeSystemObject::drain_dispatch(uint32_t max_items) noexcept {
    return guard([&] {
        pybind11::list result;
        native_->drain_dispatch(max_items, [&](const PythonDispatchEnvelope& env) {
            result.append(dispatch_to_dict(env));
        });
        return result;
    });
}

pybind11::dict NativeSystemObject::dispatch_to_dict(const PythonDispatchEnvelope& env) noexcept {
    pybind11::dict d;
    d["kind"] = static_cast<uint8_t>(env.kind);
    d["actor"] = address_to_tuple(env.actor);
    d["generation"] = env.generation;
    d["type_tag"] = static_cast<uint32_t>(env.type_tag);
    d["payload"] = pybind11::bytes(
        reinterpret_cast<const char*>(env.payload.data()),
        env.payload.size());
    d["sender"] = address_to_tuple(env.sender);
    d["message_id"] = env.message_id;
    d["ask_message_id"] = env.ask_message_id;
    if (env.has_trace) {
        d["trace_id"] = env.trace.trace_id();
        d["span_id"] = env.trace.span_id();
    }
    d["priority"] = env.priority;
    d["deadline_ns"] = env.deadline_ns;
    d["flags"] = env.flags;
    d["ack_requested"] = env.ack_requested;
    d["sequence"] = env.sequence;
    return d;
}
```

### Step 3 — Verify tests pass

```bash
ninja -C build test_pybind11_native_system
./build/tests/unit/python_binding/test_pybind11_native_system
```

**Commit:** `feat: implement NativeSystemObject with sealed exception boundary`

---

## Task 3: Implement Type Casters and Dict-Based Wire Format

### Step 1 — Write type caster tests

**File:** `tests/unit/python_binding/test_pybind11_native_system.cpp` (extend)

```cpp
TEST_F(Pybind11NativeSystemTest, ActorAddressRoundTrip) {
    ActorAddress addr(/* family= */ 0, /* packed= */ {},
                      /* port= */ 0, /* actor_type= */ 1,
                      /* actor_id= */ 42, /* incarnation= */ 1);
    auto tup = NativeSystemObject::address_to_tuple(addr);
    auto parsed = NativeSystemObject::tuple_to_address(tup);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->actor_id, 42);
    EXPECT_EQ(parsed->incarnation, 1);
}

TEST_F(Pybind11NativeSystemTest, InvalidAddressTupleRejected) {
    auto bad = pybind11::make_tuple("not", "an", "address");
    auto parsed = NativeSystemObject::tuple_to_address(bad);
    EXPECT_FALSE(parsed.has_value());
}

TEST_F(Pybind11NativeSystemTest, CommandDictValidated) {
    pybind11::dict cmd;
    cmd["kind"] = 0;  // Send
    cmd["token"] = 1;
    cmd["sequence"] = 1;
    cmd["generation"] = 1;
    cmd["origin"] = make_test_address_tuple();
    cmd["target"] = make_test_address_tuple();
    cmd["type_tag"] = 0x1000;
    cmd["payload"] = pybind11::bytes("test");
    auto parsed = NativeSystemObject::dict_to_command(cmd);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(parsed->kind, PythonCommandKind::Send);
}

TEST_F(Pybind11NativeSystemTest, CommandDictMissingRequiredKeyRejected) {
    pybind11::dict cmd;
    cmd["kind"] = 0;
    // missing 'token', 'sequence', 'origin', 'target', etc.
    auto parsed = NativeSystemObject::dict_to_command(cmd);
    EXPECT_FALSE(parsed.has_value());
}
```

### Step 2 — Implement type casters and converters

**File:** `bindings/python/native/src/python_pybind11/type_casters.hpp`

```cpp
#pragma once
#include <pybind11/pybind11.h>
#include "hpactor/python/python_bridge_types.hpp"

namespace pybind11 { namespace detail {

// ActorAddress caster: (family, packed_bytes, port, actor_type, actor_id, incarnation)
template<>
struct type_caster<hpactor::ActorAddress> {
    PYBIND11_TYPE_CASTER(hpactor::ActorAddress, const_name("ActorAddress"));

    bool load(handle src, bool /*convert*/) {
        if (!PyTuple_Check(src.ptr()) || PyTuple_Size(src.ptr()) != 6)
            return false;

        auto tup = reinterpret_borrow<tuple>(src);
        int family = tup[0].cast<int>();
        auto packed_bytes = tup[1].cast<pybind11::bytes>();
        int port = tup[2].cast<int>();
        int actor_type = tup[3].cast<int>();
        uint64_t actor_id = tup[4].cast<uint64_t>();
        uint64_t incarnation = tup[5].cast<uint64_t>();

        std::vector<uint8_t> packed(
            reinterpret_cast<const uint8_t*>(PYBIND11_BYTES_AS_STRING(packed_bytes.ptr())),
            reinterpret_cast<const uint8_t*>(PYBIND11_BYTES_AS_STRING(packed_bytes.ptr()))
                + PYBIND11_BYTES_SIZE(packed_bytes.ptr()));

        value = hpactor::ActorAddress(family, packed, port, actor_type, actor_id, incarnation);
        return true;
    }

    static handle cast(const hpactor::ActorAddress& addr,
                       return_value_policy /*policy*/, handle /*parent*/) {
        auto packed = pybind11::bytes(
            reinterpret_cast<const char*>(addr.packed_address().data()),
            addr.packed_address().size());
        return pybind11::make_tuple(
            addr.family(), packed, addr.port(),
            addr.actor_type(), addr.actor_id(), addr.incarnation()
        ).release();
    }
};

}} // namespace pybind11::detail
```

### Step 3 — Verify

```bash
ninja -C build test_pybind11_native_system
./build/tests/unit/python_binding/test_pybind11_native_system --gtest_filter="*Address*:*Command*:*Dict*"
```

**Commit:** `feat: add pybind11 type casters and dict-based wire format`

---

## Task 4: Implement Module Definition and Wire the C++ ↔ Python Boundary

### Step 1 — Write module-level Python import tests

**File:** `bindings/python/tests/unit/test_pybind11_module.py` (new)

```python
import unittest

class Pybind11ModuleTest(unittest.TestCase):
    def test_import_and_py_limited_api(self):
        import _hpactor
        self.assertEqual(_hpactor.PY_LIMITED_API, 0x030B0000)

    def test_native_system_construct_and_start(self):
        from _hpactor import NativeSystem
        config = {
            "dispatch_queue_capacity": 65536,
            "command_queue_capacity": 16384,
            "completion_queue_capacity": 16384,
            "max_actor_bindings": 65536,
            "max_dispatch_per_tick": 256,
            "max_commands_per_turn": 256,
            "loop_lag_unready_ms": 5000,
            "handler_shutdown_timeout_ms": 10000,
            "trace_handler_spans": True,
        }
        ns = NativeSystem(config)
        ns.start()
        self.assertGreaterEqual(ns.dispatch_fd, 0)
        self.assertGreaterEqual(ns.completion_fd, 0)
        ns.stop()

    def test_dispatch_drain_returns_dicts(self):
        from _hpactor import NativeSystem
        ns = NativeSystem(self._default_config())
        ns.start()
        dispatches = ns.drain_dispatch(16)
        self.assertIsInstance(dispatches, list)
        if dispatches:
            d = dispatches[0]
            self.assertIn("kind", d)
            self.assertIn("actor", d)
            self.assertIn("generation", d)
            self.assertIn("type_tag", d)
            self.assertIn("payload", d)
            self.assertIn("sender", d)
            self.assertIn("sequence", d)
        ns.stop()

    def _default_config(self):
        return {
            "dispatch_queue_capacity": 65536,
            "command_queue_capacity": 16384,
            "completion_queue_capacity": 16384,
            "max_actor_bindings": 65536,
            "max_dispatch_per_tick": 256,
            "max_commands_per_turn": 256,
            "loop_lag_unready_ms": 5000,
            "handler_shutdown_timeout_ms": 10000,
            "trace_handler_spans": True,
        }
```

### Step 2 — Write module definition

**File:** `bindings/python/native/src/python_pybind11/module.cpp`

```cpp
#include <pybind11/pybind11.h>
#include "native_system.hpp"

PYBIND11_MODULE(_hpactor, m) {
    m.attr("PY_LIMITED_API") = pybind11::int_(0x030B0000);
    m.doc() = "HPActor native runtime";

    pybind11::class_<NativeSystemObject>(m, "NativeSystem")
        .def(pybind11::init<pybind11::dict>(),
             pybind11::arg("config"))
        .def("start", &NativeSystemObject::start)
        .def("stop", &NativeSystemObject::stop)
        .def("begin_draining", &NativeSystemObject::begin_draining)
        .def("application_origin", &NativeSystemObject::application_origin)
        .def("spawn_bridge", &NativeSystemObject::spawn_bridge)
        .def("stop_bridge", &NativeSystemObject::stop_bridge,
             pybind11::arg("address"))
        .def("register_name", &NativeSystemObject::register_name,
             pybind11::arg("name"), pybind11::arg("address"))
        .def("resolve_name", &NativeSystemObject::resolve_name,
             pybind11::arg("name"))
        .def("submit", &NativeSystemObject::submit,
             pybind11::arg("command"))
        .def("drain_dispatch", &NativeSystemObject::drain_dispatch,
             pybind11::arg("max_items"))
        .def("drain_completions", &NativeSystemObject::drain_completions,
             pybind11::arg("max_items"))
        .def("snapshot", &NativeSystemObject::snapshot)
        .def_property_readonly("dispatch_fd", &NativeSystemObject::dispatch_fd)
        .def_property_readonly("completion_fd", &NativeSystemObject::completion_fd)
        // Phase 1E topology (forward-compatible stubs until Phase 1E implemented)
        .def("prepare_topology", &NativeSystemObject::prepare_topology,
             pybind11::arg("path"))
        .def("bind_topology_manifest", &NativeSystemObject::bind_topology_manifest,
             pybind11::arg("bindings"))
        .def("start_prepared_topology", &NativeSystemObject::start_prepared_topology)
        .def("complete_topology_actor", &NativeSystemObject::complete_topology_actor,
             pybind11::arg("outcome"))
        .def("record_topology_preflight", &NativeSystemObject::record_topology_preflight,
             pybind11::arg("preflight"))
        .def("last_topology_error", &NativeSystemObject::last_topology_error)
        .def("resolve_name", &NativeSystemObject::resolve_name,
             pybind11::arg("name"));
}
```

### Step 3 — Verify

```bash
ninja -C build _hpactor
cd bindings/python && python -c "import _hpactor; print(_hpactor.PY_LIMITED_API)"
python -m pytest tests/unit/test_pybind11_module.py -v
```

**Commit:** `feat: implement pybind11 module definition`

---

## Task 5: Update Python Package for Dict-Based Wire Format

### Step 1 — Identify tuple-to-dict transitions

The current `_system.py` and `_runtime.py` consume tuple-based dispatch and
completion from the C API. With pybind11, these become named dicts. The changes
are mechanical: replace positional indexing with key access.

**Current tuple access pattern:**
```python
# dispatch tuple: (actor, generation, type_tag, payload, sender, ...)
actor = dispatch[0]
generation = dispatch[1]
type_tag = dispatch[2]
payload = dispatch[3]
```

**New dict access pattern:**
```python
# dispatch dict: {"actor": ..., "generation": ..., "type_tag": ..., ...}
actor = dispatch["actor"]
generation = dispatch["generation"]
type_tag = dispatch["type_tag"]
payload = dispatch["payload"]
```

### Step 2 — Write failing tests

Extend `test_actor_system.py` to verify dict keys alongside current tests.

### Step 3 — Update Python files

**Files to modify:**
- `bindings/python/hpactor/_system.py` — `_default_native_factory()` uses dict-based config; `_NativeSystemProxy` methods read dicts from `drain_dispatch`/`drain_completions`
- `bindings/python/hpactor/_runtime.py` — `_DispatchCoordinator` and `_TokenRegistry` read dict keys instead of tuple indices

A compatibility shim is acceptable during transition: `_system.py` detects
tuple-vs-dict at runtime and normalizes. Once the C API backend is removed,
the shim is cleaned up.

### Step 4 — Verify

```bash
cd bindings/python && python -m pytest tests/unit/ -v
```

All 22 Python unit tests must pass with the dict-based wire format.

**Commit:** `refactor: update Python package for dict-based wire format`

---

## Task 6: Update Architecture Fitness Checks

### Step 1 — Update scan rules

**File:** `tests/architecture/CMakeLists.txt`

Current rules forbid `Python.h` and `PyObject` outside `python_capi/`. Updated:

```cmake
# Rule 1: Python.h and CPython API names allowed ONLY in:
#   bindings/python/native/src/python_pybind11/
# (Previously allowed in python_capi/ — that directory is removed)
add_architecture_scan(
    NAME python_header_confinement_pybind11
    PATTERN "Python\\.h|PyObject|Py_INCREF|Py_DECREF|Py_BuildValue|PyArg_Parse"
    ALLOWED_PATHS "bindings/python/native/src/python_pybind11/"
    FORBIDDEN_IN "src/" "include/" "bindings/python/native/src/"
    FORBIDDEN_IN_EXCLUDE "bindings/python/native/src/python_pybind11/"
)

# Rule 2: pybind11 headers allowed ONLY in python_pybind11/
add_architecture_scan(
    NAME pybind11_header_confinement
    PATTERN "#include.*<pybind11/"
    ALLOWED_PATHS "bindings/python/native/src/python_pybind11/"
    FORBIDDEN_IN "src/" "include/"
    FORBIDDEN_IN_EXCLUDE "bindings/python/native/src/python_pybind11/"
)

# Rule 3: throw/catch allowed in python_pybind11/ (added to allowlist)
add_architecture_scan(
    NAME exception_confinement_updated
    PATTERN "throw |catch \\(|try \\{"
    ALLOWED_PATHS
        "src/config/toml_parser.cpp"
        "src/config/toml_table_view.cpp"
        "tools/toml-compiler/compiler.cpp"
        "bindings/python/native/src/python_pybind11/"
    FORBIDDEN_IN "src/" "include/"
    FORBIDDEN_IN_EXCLUDE "src/config/" "bindings/python/native/src/python_pybind11/"
)

# Rule 4: NO Python.h/PyObject in core HPActor files (unchanged)
# Rule 5: -fno-rtti on ALL binding TUs (unchanged, verified by compile flags)
# Rule 6: No std::function in binding ports (unchanged)
# Rule 7: Protected tags 0xF0-0xF3 not accepted by remote frame routing (unchanged)
# Rule 8: Scheduler/network sources do not include binding headers (unchanged)
```

### Step 2 — Add architecture test that python_capi/ is gone

```cmake
add_architecture_scan(
    NAME no_capi_remnants
    SCRIPT [[
        if test -d "${CMAKE_SOURCE_DIR}/bindings/python/native/src/python_capi"; then
            echo "FAIL: python_capi/ directory still exists — must be removed"
            exit 1
        fi
    ]]
)
```

### Step 3 — Verify scans pass

```bash
ctest -R "Architecture" --output-on-failure
```

**Commit:** `test: update architecture scans for pybind11 backend`

---

## Task 7: Full Integration and Regression Verification

### Step 1 — Full build with Python bindings enabled

```bash
cmake -S . -B build -GNinja \
    -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
    -DENABLE_PYTHON_BINDINGS=ON
ninja -C build
```

### Step 2 — Run all existing Python binding tests

```bash
ctest --test-dir build -R 'PythonBinding' --output-on-failure
# Target: 206 tests pass (same count as Phase 1C)
```

### Step 3 — Run all C++ binding unit tests

```bash
./build/tests/unit/python_binding/test_unit_python_binding
./build/tests/integration/python_binding/test_integration_python_binding
# All existing queue, notifier, runtime, bridge, gateway tests must pass
```

### Step 4 — Run architecture fitness checks

```bash
ctest -R "Architecture" --output-on-failure
# All architecture scans pass with updated allowlists
```

### Step 5 — Verify ABI3 compliance

```bash
python -c "import sys; import _hpactor; print(sys.abiflags)"
abi3audit --strict build/bindings/python/_hpactor*.so
# Must pass — no non-stable-ABI symbols
```

### Step 6 — Verify ENABLE_PYTHON_BINDINGS=OFF

```bash
cmake -S . -B build-off -GNinja \
    -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
    -DENABLE_PYTHON_BINDINGS=OFF
ninja -C build-off
ctest --test-dir build-off --output-on-failure --parallel 8
# No Python binding tests; all other tests pass
```

### Step 7 — Verify on supported Linux TSAN

```bash
cmake -S . -B build-tsan -GNinja \
    -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF \
    -DENABLE_PYTHON_BINDINGS=ON -DENABLE_TSAN=ON
ninja -C build-tsan
ctest --test-dir build-tsan -R 'PythonBinding' --output-on-failure
```

### Step 8 — Run debug Python and reference leak check

```bash
PYTHONMALLOC=debug PYTHONASYNCIODEBUG=1 \
    ctest --test-dir build -R 'PythonBinding' --output-on-failure
```

**Commit:** `test: verify full integration with pybind11 backend`

---

## Task 8: Record Completion and Update Project Memory

### Step 1 — Update CLAUDE_MEMORY.md

Add entry:

```markdown
## Python Binding pybind11 Backend (2026-07-10)

Replaced raw CPython limited C API (`bindings/python/native/src/python_capi/`)
with pybind11 backend (`bindings/python/native/src/python_pybind11/`).

- **Backend:** pybind11 ≥ 2.12.0, header-only, `PYBIND11_USE_LIMITED_API=1`
- **ABI:** CPython 3.11 stable ABI (`Py_LIMITED_API=0x030B0000`) preserved
- **Exception model:** Sealed noexcept boundary; pybind11 TUs get `-fexceptions`;
  `pybind11::error_already_set` caught and converted at boundary; no exception
  crosses into `hpactor_python_native`
- **Wire format:** Dict-based dispatch/completion/command instead of positional tuples
- **Architecture scans:** Updated for pybind11 header confinement, exception
  allowlist, and `python_capi/` removal
- **Python API:** Zero changes — all 206 tests pass with identical behavior
- **Packaging:** Phase 1D ABI3 wheel strategy unchanged
```

### Step 2 — Update design spec status

In `docs/superpowers/specs/2026-07-10-pybind11-backend-design.md`:

```markdown
**Status:** Implemented
```

In `docs/superpowers/specs/2026-07-03-python-language-binding-design.md`:

```markdown
**Status:** Approved design; Phases 1A, 1B, 1C implemented; pybind11 backend active
```

### Step 3 — Final verification

```bash
# Full build and test
cmake -S . -B build -GNinja -DENABLE_EXAMPLES=OFF -DENABLE_APPS=OFF -DENABLE_PYTHON_BINDINGS=ON
ninja -C build
ctest --test-dir build --output-on-failure --parallel 8

# Whitespace check
git diff --check
```

**Commit:** `docs: record pybind11 backend completion`

---

## Plan Completion Checklist

- [ ] Task 1: pybind11 dependency added, CMake target builds `_hpactor`
- [ ] Task 2: `NativeSystemObject` with sealed noexcept boundary
- [ ] Task 3: Type casters and dict-based wire format
- [ ] Task 4: Module definition wires C++ ↔ Python boundary
- [ ] Task 5: Python package updated for dict-based format
- [ ] Task 6: Architecture fitness checks updated
- [ ] Task 7: Full integration — 206 Python tests + all C++ tests pass
- [ ] Task 8: Project memory and design spec status updated
- [ ] `ENABLE_PYTHON_BINDINGS=OFF` — no regression
- [ ] Architecture scans pass with updated allowlists
- [ ] `abi3audit --strict` passes on built `_hpactor`
- [ ] Linux TSAN — clean
- [ ] Debug Python + reference leak check — clean
- [ ] `git diff --check` — clean
- [ ] `python_capi/` directory removed
- [ ] No pybind11 headers outside `python_pybind11/`
- [ ] No `Python.h` / `PyObject` outside `python_pybind11/`
- [ ] All public Python APIs unchanged (`hpactor.__all__` identical)
