// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include <memory>
#include <optional>
#include <string>

#include <pybind11/pybind11.h>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/python/python_bridge_types.hpp>
#include <hpactor/python/python_native_system.hpp>
#include <hpactor/python/python_runtime_config.hpp>
#include <hpactor/ref/actor_ref.hpp>

/// \brief pybind11-compatible wrapper around PythonNativeSystem.
///
/// Owns a \c PythonNativeSystem* and exposes every method as noexcept.
/// pybind11 errors (\c error_already_set) are caught internally by the
/// \c guard() helper and converted to sentinel values; they never propagate
/// to the exception-free native bridge.
class NativeSystemObject {
  public:
    /// \brief Construct from a Python config dict.
    ///
    /// Validates config keys, constructs the native system.  Throws
    /// pybind11::value_error / pybind11::runtime_error on invalid config.
    /// Callers MUST wrap this in guard() or catch the exception.
    explicit NativeSystemObject(pybind11::dict config);

    ~NativeSystemObject();

    NativeSystemObject(const NativeSystemObject&) = delete;
    NativeSystemObject& operator=(const NativeSystemObject&) = delete;

    // ── Lifecycle ────────────────────────────────────────────────────

    bool start() noexcept;
    bool stop() noexcept;
    bool begin_draining() noexcept;

    // ── Actor management ─────────────────────────────────────────────

    pybind11::object application_origin() noexcept;
    pybind11::object spawn_bridge() noexcept;
    bool stop_bridge(pybind11::tuple address) noexcept;
    bool register_name(const std::string& name, pybind11::tuple address) noexcept;
    pybind11::object resolve_name(const std::string& name) noexcept;

    // ── Message passing ──────────────────────────────────────────────

    bool submit(pybind11::dict command) noexcept;
    pybind11::list drain_dispatch(uint32_t max_items) noexcept;
    pybind11::list drain_completions(uint32_t max_items) noexcept;

    // ── Observability ────────────────────────────────────────────────

    pybind11::dict snapshot() noexcept;
    int dispatch_fd() const noexcept;
    int completion_fd() const noexcept;

    // ── Topology (Phase 1E — forward-compatible stubs) ───────────────

    pybind11::object prepare_topology(const std::string& path) noexcept;
    bool bind_topology_manifest(pybind11::list bindings) noexcept;
    bool start_prepared_topology() noexcept;
    bool complete_topology_actor(pybind11::dict outcome) noexcept;
    bool record_topology_preflight(pybind11::dict preflight) noexcept;
    pybind11::object last_topology_error() noexcept;

  private:
    /// \brief Sealed noexcept boundary.
    ///
    /// Executes \p f inside a try/catch for pybind11::error_already_set.
    /// On exception, restores the Python error state and returns the
    /// appropriate sentinel for the return type.
    template <typename F> auto guard(F&& f) noexcept -> decltype(f()) {
        using Ret = decltype(f());
        try {
            return f();
        } catch (pybind11::error_already_set& e) {
            e.restore();
            if constexpr (std::is_void_v<Ret>) {
                return;
            } else if constexpr (std::is_same_v<Ret, bool>) {
                return false;
            } else {
                return Ret{};
            }
        }
    }

    // ── Conversion helpers (value-only, no PyObject* stored) ─────────

    static pybind11::dict
    dispatch_to_dict(const hpactor::python::PythonDispatchEnvelope& env) noexcept;
    static pybind11::dict
    completion_to_dict(const hpactor::python::PythonCompletion& comp) noexcept;
    static pybind11::tuple
    address_to_tuple(const hpactor::ActorAddress& addr) noexcept;
    static std::optional<hpactor::ActorAddress>
    tuple_to_address(pybind11::tuple tup) noexcept;
    static std::optional<hpactor::python::PythonCommand>
    dict_to_command(pybind11::dict cmd_dict) noexcept;
    static hpactor::python::PythonRuntimeConfig
    dict_to_runtime_config(pybind11::dict cfg);

    std::unique_ptr<hpactor::python::PythonNativeSystem> native_;
};
