// Copyright 2026 HPActor Contributors (Apache 2.0)
#pragma once
// CPython limited-API conversion helpers.  Python.h confined to this directory.
#define Py_LIMITED_API 0x030B0000
#include <Python.h>

#include <hpactor/python/python_native_system.hpp>

namespace hpactor::python::capi {

/// Convert a Python config dict to a PythonRuntimeConfig.
/// Returns false and sets a Python exception on error.
bool parse_config(PyObject* dict, PythonRuntimeConfig& out) noexcept;

/// Build an address tuple: (family, packed_addr_bytes, port, actor_type,
/// actor_id, incarnation).
PyObject* address_to_tuple(const ActorAddress& addr);

/// Build a dispatch tuple:
/// (actor, generation, type_tag, payload, sender, message_id, ...)
PyObject* dispatch_to_tuple(const PythonDispatchEnvelope& env);

/// Build a completion tuple.
PyObject* completion_to_tuple(const PythonCompletion& comp);

} // namespace hpactor::python::capi
