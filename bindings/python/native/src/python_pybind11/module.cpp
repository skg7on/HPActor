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

#include <pybind11/pybind11.h>

#include "native_system.hpp"

namespace py = pybind11;

PYBIND11_MODULE(_hpactor, m) {
    m.attr("PY_LIMITED_API") = py::int_(0); // full API; ABI3 to follow
    m.doc() = "HPActor native runtime";

    py::class_<NativeSystemObject>(m, "NativeSystem")
        .def(py::init<py::dict>(), py::arg("config"))
        .def("start", &NativeSystemObject::start)
        .def("stop", &NativeSystemObject::stop)
        .def("begin_draining", &NativeSystemObject::begin_draining)
        .def("application_origin", &NativeSystemObject::application_origin)
        .def("spawn_bridge", &NativeSystemObject::spawn_bridge)
        .def("stop_bridge", &NativeSystemObject::stop_bridge, py::arg("address"))
        .def("register_name", &NativeSystemObject::register_name,
             py::arg("name"), py::arg("address"))
        .def("resolve_name", &NativeSystemObject::resolve_name, py::arg("name"))
        .def("submit", &NativeSystemObject::submit, py::arg("command"))
        .def("drain_dispatch", &NativeSystemObject::drain_dispatch,
             py::arg("max_items"))
        .def("drain_completions", &NativeSystemObject::drain_completions,
             py::arg("max_items"))
        .def("snapshot", &NativeSystemObject::snapshot)
        .def_property_readonly("dispatch_fd", &NativeSystemObject::dispatch_fd)
        .def_property_readonly("completion_fd", &NativeSystemObject::completion_fd)
        // Phase 1E topology stubs
        .def("prepare_topology", &NativeSystemObject::prepare_topology,
             py::arg("path"))
        .def("bind_topology_manifest",
             &NativeSystemObject::bind_topology_manifest, py::arg("bindings"))
        .def("start_prepared_topology", &NativeSystemObject::start_prepared_topology)
        .def("complete_topology_actor",
             &NativeSystemObject::complete_topology_actor, py::arg("outcome"))
        .def("record_topology_preflight",
             &NativeSystemObject::record_topology_preflight, py::arg("preflight"))
        .def("last_topology_error", &NativeSystemObject::last_topology_error)
        .def("resolve_name", &NativeSystemObject::resolve_name, py::arg("name"));
}
