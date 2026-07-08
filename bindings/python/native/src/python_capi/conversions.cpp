// Copyright 2026 HPActor Contributors (Apache 2.0)
#define Py_LIMITED_API 0x030B0000
#include "conversions.hpp"

namespace hpactor::python::capi {

bool parse_config(PyObject* dict, PythonRuntimeConfig& out) noexcept {
    if (!dict || !PyDict_Check(dict)) {
        PyErr_SetString(PyExc_TypeError, "config must be a dict");
        return false;
    }
    // Accept subset of known keys; missing keys keep defaults.
    // Stub: accept the default config for now.
    (void)dict;
    return true;
}

PyObject* address_to_tuple(const ActorAddress& addr) {
    // Stub: return an empty tuple for now.
    return PyTuple_New(0);
}

PyObject* dispatch_to_tuple(const PythonDispatchEnvelope& env) {
    (void)env;
    return PyTuple_New(0);
}

PyObject* completion_to_tuple(const PythonCompletion& comp) {
    (void)comp;
    return PyTuple_New(0);
}

} // namespace hpactor::python::capi
