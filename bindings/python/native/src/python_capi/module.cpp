// Copyright 2026 HPActor Contributors (Apache 2.0)
#define Py_LIMITED_API 0x030B0000
#include "native_system_type.hpp"
#include <Python.h>
#include <hpactor/python/python_native_system.hpp>

static PyMethodDef hpactor_methods[] = {
    {nullptr, nullptr, 0, nullptr} // sentinel
};

static struct PyModuleDef hpactor_module = {
    PyModuleDef_HEAD_INIT,
    "_hpactor",
    "HPActor native Python binding (limited API)",
    -1,
    hpactor_methods,
    nullptr, // m_slots
    nullptr, // m_traverse
    nullptr, // m_clear
    nullptr, // m_free
};

PyMODINIT_FUNC PyInit__hpactor(void) {
    auto* mod = PyModule_Create(&hpactor_module);
    if (!mod)
        return nullptr;

    // Exported version constant.
    PyModule_AddObject(mod, "PY_LIMITED_API", PyLong_FromUnsignedLong(0x030B0000));

    // Register the NativeSystem heap type.
    auto* ns_type = hpactor::python::capi::init_native_system_type(mod);
    if (!ns_type) {
        Py_DECREF(mod);
        return nullptr;
    }

    return mod;
}
