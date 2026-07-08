// Copyright 2026 HPActor Contributors (Apache 2.0)
#define Py_LIMITED_API 0x030B0000
#include "native_system_type.hpp"
#include "conversions.hpp"
#include <new>

namespace hpactor::python::capi {

// ── NativeSystem dealloc ─────────────────────────────────────────────────
static void native_system_dealloc(PyObject* self) {
    auto* ns = reinterpret_cast<NativeSystemObject*>(self);
    if (ns->native) {
        (void)ns->native->stop();
        delete ns->native;
        ns->native = nullptr;
    }
    Py_TYPE(self)->tp_free(self);
}

// ── NativeSystem.__init__ ────────────────────────────────────────────────
static int native_system_init(PyObject* self, PyObject* args, PyObject* kwargs) {
    auto* ns = reinterpret_cast<NativeSystemObject*>(self);

    PyObject* config_dict = nullptr;
    static const char* kwlist[] = {"config", nullptr};
    if (!PyArg_ParseTupleAndKeywords(args, kwargs, "|O",
                                     const_cast<char**>(kwlist), &config_dict)) {
        return -1;
    }

    PythonRuntimeConfig py_config;
    if (config_dict && !parse_config(config_dict, py_config)) {
        return -1;
    }

    Config sys_config;
    sys_config.scheduler_threads = 0;
    sys_config.enable_network = false;

    auto created =
        PythonNativeSystem::create(std::move(sys_config), std::move(py_config));
    if (!created.ok()) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create native system");
        return -1;
    }

    ns->native = created.value().release();
    return 0;
}

// ── NativeSystem.start ───────────────────────────────────────────────────
static PyObject* native_system_start(PyObject* self, PyObject* /*args*/) {
    auto* ns = reinterpret_cast<NativeSystemObject*>(self);
    if (!ns->native) {
        PyErr_SetString(PyExc_RuntimeError, "native system not created");
        return nullptr;
    }
    auto result = ns->native->start();
    if (!result.ok()) {
        PyErr_SetString(PyExc_RuntimeError, "failed to start");
        return nullptr;
    }
    Py_RETURN_NONE;
}

// ── NativeSystem.stop ────────────────────────────────────────────────────
static PyObject* native_system_stop(PyObject* self, PyObject* /*args*/) {
    auto* ns = reinterpret_cast<NativeSystemObject*>(self);
    if (ns->native) {
        (void)ns->native->stop();
    }
    Py_RETURN_NONE;
}

// ── NativeSystem.application_origin ──────────────────────────────────────
static PyObject*
native_system_application_origin(PyObject* self, PyObject* /*args*/) {
    auto* ns = reinterpret_cast<NativeSystemObject*>(self);
    if (!ns->native) {
        PyErr_SetString(PyExc_RuntimeError, "native system not started");
        return nullptr;
    }
    auto addr = ns->native->application_origin();
    auto gen = ns->native->application_generation();
    // Return (address_tuple, generation).
    PyObject* tup = PyTuple_New(2);
    if (!tup)
        return nullptr;
    PyTuple_SetItem(tup, 0, address_to_tuple(addr));
    PyTuple_SetItem(tup, 1, PyLong_FromUnsignedLongLong(gen));
    return tup;
}

// ── NativeSystem.dispatch_fd ─────────────────────────────────────────────
static PyObject* native_system_dispatch_fd(PyObject* self, void* /*closure*/) {
    auto* ns = reinterpret_cast<NativeSystemObject*>(self);
    if (!ns->native)
        return PyLong_FromLong(-1);
    return PyLong_FromLong(ns->native->dispatch_read_fd());
}

// ── NativeSystem.completion_fd ───────────────────────────────────────────
static PyObject* native_system_completion_fd(PyObject* self, void* /*closure*/) {
    auto* ns = reinterpret_cast<NativeSystemObject*>(self);
    if (!ns->native)
        return PyLong_FromLong(-1);
    return PyLong_FromLong(ns->native->completion_read_fd());
}

// ── Methods table ────────────────────────────────────────────────────────
static PyMethodDef native_system_methods[] = {
    {"start", native_system_start, METH_NOARGS, "Start the native system."},
    {"stop", native_system_stop, METH_NOARGS, "Stop the native system (idempotent)."},
    {"application_origin", native_system_application_origin, METH_NOARGS,
     "Return (address_tuple, generation) for the application bridge."},
    {nullptr, nullptr, 0, nullptr} // sentinel
};

static PyGetSetDef native_system_getset[] = {
    {"dispatch_fd", native_system_dispatch_fd, nullptr,
     "Read-end fd for the dispatch notifier.", nullptr},
    {"completion_fd", native_system_completion_fd, nullptr,
     "Read-end fd for the completion notifier.", nullptr},
    {nullptr} // sentinel
};

// ── Type spec ────────────────────────────────────────────────────────────
static PyType_Slot native_system_slots[] = {
    {Py_tp_dealloc, (void*)native_system_dealloc},
    {Py_tp_init, (void*)native_system_init},
    {Py_tp_methods, (void*)native_system_methods},
    {Py_tp_getset, (void*)native_system_getset},
    {0, nullptr}};

static PyType_Spec native_system_spec = {
    "_hpactor.NativeSystem",
    sizeof(NativeSystemObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE,
    native_system_slots,
};

PyTypeObject* init_native_system_type(PyObject* module) noexcept {
    auto* type = (PyTypeObject*)PyType_FromSpec(&native_system_spec);
    if (!type)
        return nullptr;
    if (PyModule_AddObject(module, "NativeSystem", (PyObject*)type) < 0) {
        Py_DECREF(type);
        return nullptr;
    }
    return type;
}

} // namespace hpactor::python::capi
