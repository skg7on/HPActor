// Copyright 2026 HPActor Contributors (Apache 2.0)
#pragma once
#define Py_LIMITED_API 0x030B0000
#include <Python.h>

#include <hpactor/python/python_native_system.hpp>

namespace hpactor::python::capi {

/// Heap-type object wrapping one PythonNativeSystem*.
struct NativeSystemObject {
    PyObject_HEAD PythonNativeSystem* native{nullptr};
};

/// Create the heap type (called once during module init).
PyTypeObject* init_native_system_type(PyObject* module) noexcept;

} // namespace hpactor::python::capi
