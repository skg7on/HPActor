# Python Binding pybind11 Backend Design

**Status:** Approved design
**Date:** 2026-07-10
**Target:** Replace raw CPython limited C API with pybind11 as the Python binding backend
**Depends on:** Phases 1A, 1B, 1C implemented; Phase 1D packaging plan

## 1. Summary

Replace the raw CPython limited C API backend (`bindings/python/native/src/python_capi/`)
with a pybind11-based backend (`bindings/python/native/src/python_pybind11/`). The
new backend produces the same `_hpactor` native module, implements the same internal
interfaces, and preserves every architecture invariant: ABI3 compatibility, bounded
queues, generation fencing, GIL isolation, and `-fno-rtti` on all binding TUs.

The pybind11 TUs are granted a narrow exception allowlist (matching the existing
pattern for `toml_parser.cpp`) with a sealed noexcept boundary that prevents any
C++ exception from crossing into `hpactor_python_native`.

## 2. Motivation

Raw CPython C API requires verbose, error-prone manual reference counting, explicit
`Py_BuildValue`/`PyArg_ParseTuple` calls, and `PyType_FromSpec` boilerplate.
pybind11 provides:

- Automatic reference counting via `pybind11::object` and `pybind11::handle`
- Declarative type registration via `pybind11::class_` and `PYBIND11_MODULE`
- Built-in STL type casters (`std::string`, `std::vector`, `std::map`, `std::optional`)
- Compile-time checked method registration
- Same ABI3 compatibility when configured with `PYBIND11_USE_LIMITED_API`

## 3. Architecture

### 3.1 Execution Domains (unchanged)

Three execution domains remain identical:

1. **HPActor scheduler workers** — execute C++ actors; never call Python or acquire GIL
2. **Python actor runtime thread** — owns the dedicated asyncio loop, Python actor
   instances, behaviors, and Python-side registries
3. **Python application thread/loop** — owns the user's application coroutine and
   public `ActorSystem` proxy

### 3.2 Message Flows (unchanged)

Inbound: `DeliveryPipeline` → `PythonBridgeActor::receive()` → `PythonDispatchEnvelope`
→ bounded dispatch queue → asyncio reader → actor runner.

Outbound: Python handler serializes → `PythonCommand` → bounded command queue →
`PythonGatewayActor` (budgeted) → protected F1/F2 control messages → originating
bridge actor.

The pybind11 backend only changes how the Python-side entry points are implemented
— how `NativeSystem` methods are exposed to Python, how type conversions happen,
and how the module is initialized.

### 3.3 Exception Boundary

```
┌─────────────────────────────────────────────────┐
│ src/python_pybind11/  (exceptions ALLOWED)       │
│                                                   │
│  pybind11::module_, pybind11::class_              │
│  Type casters                                     │
│  throw pybind11::error_already_set               │
│                                                   │
│  ┌─────────────────────────────────────────────┐ │
│  │  noexcept boundary layer                    │ │
│  │  Every method called from native bridge     │ │
│  │  is noexcept. Internal try/catch converts   │ │
│  │  pybind11::error_already_set → error codes. │ │
│  └─────────────────────────────────────────────┘ │
└──────────────────┬──────────────────────────────┘
                   │ error codes / sentinels only
┌──────────────────▼──────────────────────────────┐
│ hpactor_python_native  (NO exceptions, NO RTTI) │
│  PythonRuntime, PythonBridgeActor, etc.          │
│  No Python.h, no PyObject*                       │
└──────────────────────────────────────────────────┘
```

**Key rules:**

1. Every function exposed from `python_pybind11` to the native bridge is `noexcept`
   and returns explicit error values.
2. `pybind11::error_already_set` is caught inside `python_pybind11` TUs and converted
   to error sentinels; it never propagates to `hpactor_python_native`.
3. pybind11 may use exceptions freely for internal control flow (type casting errors,
   argument parsing failures) — these are caught at the module boundary.
4. The existing `CompletionPort<T>` function-pointer pattern is unchanged.

### 3.4 Compilation Model

```cmake
# Only for bindings/python/native/src/python_pybind11/
target_compile_options(hpactor_python_pybind11 PRIVATE
    -fexceptions          # allow exceptions in this TU only
    -fno-rtti             # RTTI still banned
)
target_compile_definitions(hpactor_python_pybind11 PRIVATE
    Py_LIMITED_API=0x030B0000
    PYBIND11_USE_LIMITED_API=1
)
target_link_libraries(hpactor_python_pybind11
    PRIVATE pybind11::headers    # header-only
    PRIVATE hpactor_python_native
)
```

## 4. Component Design

### 4.1 Module Definition (`module.cpp`)

Uses `PYBIND11_MODULE` instead of raw `PyModuleDef` + `PyInit__hpactor`:

```cpp
PYBIND11_MODULE(_hpactor, m) {
    m.attr("PY_LIMITED_API") = pybind11::int_(0x030B0000);
    m.doc() = "HPActor native runtime";

    pybind11::class_<NativeSystemObject>(m, "NativeSystem")
        .def(pybind11::init<pybind11::dict>())
        .def("start", &NativeSystemObject::start)
        .def("stop", &NativeSystemObject::stop)
        .def("begin_draining", &NativeSystemObject::begin_draining)
        .def("application_origin", &NativeSystemObject::application_origin)
        .def("spawn_bridge", &NativeSystemObject::spawn_bridge)
        .def("stop_bridge", &NativeSystemObject::stop_bridge)
        .def("register_name", &NativeSystemObject::register_name)
        .def("submit", &NativeSystemObject::submit)
        .def("drain_dispatch", &NativeSystemObject::drain_dispatch)
        .def("drain_completions", &NativeSystemObject::drain_completions)
        .def("snapshot", &NativeSystemObject::snapshot)
        .def_property_readonly("dispatch_fd", &NativeSystemObject::dispatch_fd)
        .def_property_readonly("completion_fd", &NativeSystemObject::completion_fd)
        // Phase 1E topology methods
        .def("prepare_topology", &NativeSystemObject::prepare_topology)
        .def("bind_topology_manifest", &NativeSystemObject::bind_topology_manifest)
        .def("start_prepared_topology", &NativeSystemObject::start_prepared_topology)
        .def("complete_topology_actor", &NativeSystemObject::complete_topology_actor)
        .def("record_topology_preflight", &NativeSystemObject::record_topology_preflight)
        .def("last_topology_error", &NativeSystemObject::last_topology_error)
        .def("resolve_name", &NativeSystemObject::resolve_name);
}
```

`PYBIND11_MODULE` does not throw at definition time — only at call time.

### 4.2 NativeSystemObject Wrapper (`native_system.hpp/.cpp`)

A C++ class that owns a `PythonNativeSystem*` and exposes noexcept methods for
pybind11 to bind. Every method that touches pybind11 types wraps its body in a
noexcept guard that catches `pybind11::error_already_set`.

```cpp
class NativeSystemObject {
public:
    explicit NativeSystemObject(pybind11::dict config);
    ~NativeSystemObject();

    // All of these are noexcept — pybind11 errors caught internally
    bool start() noexcept;
    bool stop() noexcept;
    bool begin_draining() noexcept;
    pybind11::object application_origin() noexcept;
    pybind11::object spawn_bridge() noexcept;
    bool stop_bridge(pybind11::tuple address) noexcept;
    bool register_name(const std::string& name, pybind11::tuple address) noexcept;
    bool submit(pybind11::dict command) noexcept;
    pybind11::list drain_dispatch(uint32_t max_items) noexcept;
    pybind11::list drain_completions(uint32_t max_items) noexcept;
    pybind11::dict snapshot() noexcept;
    int dispatch_fd() const noexcept;
    int completion_fd() const noexcept;

    // Phase 1E topology methods
    pybind11::object prepare_topology(const std::string& path) noexcept;
    bool bind_topology_manifest(pybind11::list bindings) noexcept;
    bool start_prepared_topology() noexcept;
    bool complete_topology_actor(pybind11::dict outcome) noexcept;
    bool record_topology_preflight(pybind11::dict preflight) noexcept;
    pybind11::object last_topology_error() noexcept;
    pybind11::object resolve_name(const std::string& name) noexcept;

private:
    std::unique_ptr<PythonNativeSystem> native_;

    // Internal noexcept guard — catches pybind11 errors, restores Python error state,
    // and returns sentinel values
    template<typename F>
    auto guard(F&& f) noexcept -> decltype(f()) {
        try { return f(); }
        catch (pybind11::error_already_set& e) {
            e.restore();
            // Return appropriate sentinel: false for bool, none for pybind11::object,
            // empty list for list, etc.
            if constexpr (std::is_same_v<decltype(f()), bool>) return false;
            else if constexpr (std::is_same_v<decltype(f()), pybind11::object>) return {};
            else if constexpr (std::is_same_v<decltype(f()), pybind11::list>) return {};
            else if constexpr (std::is_same_v<decltype(f()), pybind11::dict>) return {};
            else if constexpr (std::is_same_v<decltype(f()), pybind11::tuple>) return {};
            else return {};
        }
    }
};
```

**Destruction ordering:** `native_` is destroyed before pybind11's interpreter
state is torn down. Pybind11 manages object lifetimes through the CPython limited
API — no direct struct field access, no borrowed references stored across calls.

### 4.3 Type Casters and Conversions

**Current C API tuple schemas → pybind11 equivalents:**

| Current C API | pybind11 Replacement | Direction |
|---|---|---|
| Config as raw `PyObject*` dict → `parse_config()` | `pybind11::dict` → validated struct construction in `NativeSystemObject` constructor | Python → C++ |
| `address_to_tuple()` → `(family, packed, port, type, id, inc)` | Custom `type_caster<ActorAddress>` auto-converting to/from `(int, bytes, int, int, int, int)` tuple | Both |
| `dispatch_to_tuple()` → 13-field tuple | `pybind11::dict` with named keys, built in `drain_dispatch()` loop | C++ → Python |
| `completion_to_tuple()` → 14-field tuple | `pybind11::dict` with named keys, built in `drain_completions()` loop | C++ → Python |
| `submit(command_tuple)` → 22-field tuple | `pybind11::dict` validated in `submit()`, converted to `PythonCommand` | Python → C++ |
| `snapshot_to_dict()` → plain dict | `pybind11::dict` built from `PythonRuntimeSnapshot` fields | C++ → Python |

Named dicts replace positional tuples for dispatch, completion, and command types.
This is a deliberate interface improvement — named keys are self-documenting and
more robust against field reordering. The interface remains internal (the
pure-Python `hpactor/` package is the only consumer).

**Custom type caster for `ActorAddress`:**

```cpp
namespace pybind11 { namespace detail {
template<>
struct type_caster<hpactor::ActorAddress> {
    PYBIND11_TYPE_CASTER(hpactor::ActorAddress, const_name("ActorAddress"));

    bool load(handle src, bool);
    static handle cast(const hpactor::ActorAddress& addr, return_value_policy, handle);
};
}}
```

The caster serializes to `(family: int, packed_address: bytes, port: int,
actor_type: int, actor_id: int, incarnation: int)`. All fields are CPython
limited-API-compatible (no struct field access).

### 4.4 Phase 1E Topology Types

The topology interface uses the same pybind11 patterns:

- `prepare_topology(path)` returns a `list[dict]` of topology descriptors, where
  each dict contains `topology_index`, `kind` (0=Cpp, 1=Python), `behavior` string,
  and `factory_token` (0 for C++ actors).
- `bind_topology_manifest(bindings)` accepts `list[tuple]` of `(topology_index,
  factory_token, args_fingerprint)`, validates against prepared topology, returns
  `bool`.
- `complete_topology_actor(outcome)` accepts `dict` with `factory_token`, `actor`
  (address tuple), `generation`, `outcome` (int enum), `detail` (str).
- `last_topology_error()` returns `dict` with `phase` (int), `actor_id` (int, 0
  if none), `behavior` (str), `code` (int), `detail` (str), `rollback_bits` (int),
  or `None` when no error.

These match the Phase 1E design spec exactly — only the transport format changes
from tuples to dicts.

## 5. ABI3 Constraints

`PYBIND11_USE_LIMITED_API=1` + `Py_LIMITED_API=0x030B0000` imposes constraints:

- **No direct struct field access.** `Py_TYPE()`, `Py_REFCNT()`, `Py_SIZE()` macros
  are disallowed. pybind11 2.12+ internally uses the stable-ABI function equivalents
  (`PyObject_Type()`, `PyObject_GetRefCount()`).
- **`PyType_FromSpec` only.** Custom types use `PyType_FromSpec` under the hood;
  pybind11's `class_` handles this automatically.
- **No `tp_name` direct access.** Accessed through `PyType_GetSlot()`.
- **pybind11 version requirement:** ≥ 2.12.0 (first release with full stable-ABI
  support).

These constraints are enforced at build time — `PYBIND11_USE_LIMITED_API` triggers
compile-time checks in pybind11 headers.

## 6. Architecture Scan Updates

Current architecture fitness checks are updated:

| Rule | Current | Updated |
|---|---|---|
| `Python.h` / `PyObject` allowed in | `bindings/python/native/src/python_capi/` | `bindings/python/native/src/python_pybind11/` |
| `pybind11/` headers allowed in | (n/a — no pybind11 used) | `bindings/python/native/src/python_pybind11/` only |
| `throw` / `catch` allowed in | `src/config/toml_parser.cpp`, `src/config/toml_table_view.cpp`, `tools/toml-compiler/compiler.cpp` | + `bindings/python/native/src/python_pybind11/*.cpp` added to allowlist |
| `-fexceptions` allowed for | 2 TUs | 2 TUs + `hpactor_python_pybind11` target |
| `-fno-rtti` on binding TUs | unchanged | unchanged (pybind11 does not require RTTI) |
| `std::function` in binding ports | forbidden | unchanged |
| Protected tags 0xF0–0xF3 rejected from remote ingress | unchanged | unchanged |
| Scheduler/network sources include Python binding headers | forbidden | forbidden (same as current) |
| `Python.h` / `PyObject` in core HPActor files | forbidden | unchanged |

## 7. Updated Alternatives Assessment

The original design spec (2026-07-03) Section 20 explicitly rejected pybind11:

> *"Nanobind or pybind11 — Both offer productive C++ wrappers, but their normal
> binding model relies on C++ exception support. The repository's exception
> allowlist excludes binding translation units."*

**This decision is reversed.** pybind11 is now the selected backend. The earlier
concern about exceptions is addressed by:

1. A **sealed noexcept boundary** — `pybind11::error_already_set` is caught inside
   `python_pybind11/` TUs and never propagates to `hpactor_python_native` or any
   scheduler thread.
2. A **narrow exception allowlist** — only files under `src/python_pybind11/` are
   granted `-fexceptions`, matching the existing pattern for `toml_parser.cpp`.
3. **ABI3 compatibility** — `PYBIND11_USE_LIMITED_API` preserves the Phase 1D
   `cp311-abi3` wheel strategy.

## 8. Non-Goals

This design does NOT:

- Change the pure-Python `hpactor/` package API
- Change any native bridge type, queue, actor, runtime, reliability, or observability code
- Change the Phase 1D wheel packaging strategy (still 4 ABI3 wheels + universal client wheel)
- Change the Phase 1E declarative topology behavior
- Change the Phase 2 external SDK
- Introduce nanobind (rejected for the same exception-model reasons as the original pybind11 rejection)
- Add RTTI to any binding TU
- Allow `Python.h` or `pybind11/` headers outside `python_pybind11/`
- Support Windows

## 9. Testing Strategy

### 9.1 Python-Level Contract Tests

The existing 206 Python binding tests must pass unchanged. The Python-facing API
(`ActorSystem`, `Actor`, `Behavior`, `MessageRegistry`, delivery types, exceptions)
is identical — pybind11 changes only the internal transport format (tuples → dicts).

### 9.2 C++ Unit Tests

New tests for the pybind11-specific code:

- **`NativeSystemObject` construction:** valid config dict → successful construction;
  invalid config values → `ValueError` set on Python error state.
- **`guard()` wrapper:** simulated pybind11 error → sentinel returned, Python error
  state preserved.
- **`ActorAddress` type caster:** round-trip `ActorAddress` ↔ Python tuple; invalid
  tuple shapes → `TypeError`.
- **Dispatch/completion dicts:** verify named keys match expected schema; verify
  values match current tuple-based output.

### 9.3 Architecture Containment

Updated architecture scans (Section 6) enforced in CI.

### 9.4 Build Verification

- Default `HPACTOR_PYTHON_BINDING_BACKEND=pybind11` → pybind11 backend compiles,
  `python_capi/` directory excluded from build.
- `ENABLE_PYTHON_BINDINGS=OFF` → no Python binding code compiled (unchanged).
- Full `ctest` with Python binding tests passes.

## 10. Acceptance Criteria

1. The `_hpactor` module built with pybind11 passes all 206 existing Python binding tests.
2. No `Python.h`, `PyObject`, `Py_BuildValue`, `PyArg_ParseTuple`, `PyType_FromSpec`
   remain in production binding code outside `python_pybind11/`.
3. No pybind11 header is included outside `bindings/python/native/src/python_pybind11/`.
4. No C++ exception propagates from `python_pybind11/` into `hpactor_python_native`.
5. `-fno-rtti` is enforced on all binding TUs including `python_pybind11/`.
6. The built `.so`/`.dylib` uses only CPython stable ABI symbols (verified by `abi3audit`).
7. Architecture fitness checks pass with updated allowlists.
8. Build works on Linux x86_64/ARM64 and macOS x86_64/ARM64.
9. All existing C++ binding unit tests (queues, notifier, runtime, bridge, gateway,
   reliability, config, codec, router) continue to pass.
10. The pure-Python `hpactor/` package requires zero changes.

## 11. Phase Impact

| Phase | Impact |
|---|---|
| 1A (native foundation) | None — bridge types and queues unchanged |
| 1B (actor API) | **This phase's C API layer is replaced** |
| 1C (reliability) | None — reliability controller unchanged |
| 1D (packaging) | Adds pybind11 ≥ 2.12.0 header-only dependency; ABI3 wheel strategy preserved |
| 1E (topology) | Implementation plan for 1E must use dict-based topology interfaces (already in this spec) |
| 2 (external SDK) | None |

## 12. References

- [Python language binding design](2026-07-03-python-language-binding-design.md) — umbrella architecture (Section 20 updated by this design)
- [Phase 1A plan](../plans/2026-07-03-python-binding-phase1a-native-foundation.md)
- [Phase 1B plan](../plans/2026-07-05-python-binding-phase1b-actor-api.md) — C API implementation tasks replaced by this design
- [Phase 1C plan](../plans/2026-07-05-python-binding-phase1c-reliability-operations.md)
- [Phase 1D plan](../plans/2026-07-05-python-binding-phase1d-packaging-release.md)
- [Phase 1E design](2026-07-06-python-binding-phase1e-declarative-topology-design.md)
- [Phase 2 design](2026-07-06-python-binding-phase2-external-sdk-design.md)
