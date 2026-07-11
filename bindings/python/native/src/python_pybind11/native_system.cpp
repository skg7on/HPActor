// Copyright 2026 HPActor Contributors (Apache 2.0)
//
// Licensed under the Apache License, Version 2.0.

#include "native_system.hpp"

#include <hpactor/python/python_native_system.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

namespace py = pybind11;

// ═══════════════════════════════════════════════════════════════════════
// Config conversion
// ═══════════════════════════════════════════════════════════════════════

hpactor::python::PythonRuntimeConfig
NativeSystemObject::dict_to_runtime_config(py::dict cfg) {
    hpactor::python::PythonRuntimeConfig rt;
    try {
        if (cfg.contains("dispatch_queue_capacity"))
            rt.dispatch_queue_capacity =
                cfg["dispatch_queue_capacity"].cast<uint32_t>();
        if (cfg.contains("command_queue_capacity"))
            rt.command_queue_capacity =
                cfg["command_queue_capacity"].cast<uint32_t>();
        if (cfg.contains("completion_queue_capacity"))
            rt.completion_queue_capacity =
                cfg["completion_queue_capacity"].cast<uint32_t>();
        if (cfg.contains("max_actor_bindings"))
            rt.max_actor_bindings = cfg["max_actor_bindings"].cast<uint32_t>();
        if (cfg.contains("max_dispatch_per_tick"))
            rt.max_dispatch_per_tick =
                cfg["max_dispatch_per_tick"].cast<uint32_t>();
        if (cfg.contains("max_commands_per_turn"))
            rt.max_commands_per_turn =
                cfg["max_commands_per_turn"].cast<uint32_t>();
        if (cfg.contains("loop_lag_unready_ms"))
            rt.loop_lag_unready_ms = cfg["loop_lag_unready_ms"].cast<uint32_t>();
        if (cfg.contains("handler_shutdown_timeout_ms"))
            rt.handler_shutdown_timeout_ms =
                cfg["handler_shutdown_timeout_ms"].cast<uint32_t>();
        if (cfg.contains("trace_handler_spans"))
            rt.trace_handler_spans = cfg["trace_handler_spans"].cast<bool>();
    } catch (const py::cast_error& e) {
        throw py::type_error(std::string("invalid config value type: ") + e.what());
    }
    return rt;
}

// ═══════════════════════════════════════════════════════════════════════
// Constructor / destructor
// ═══════════════════════════════════════════════════════════════════════

NativeSystemObject::NativeSystemObject(py::dict config) {
    auto rt_config = dict_to_runtime_config(config);
    auto err = rt_config.validate();
    if (err != hpactor::python::PythonConfigError::None) {
        throw py::value_error("invalid Python binding config");
    }

    auto sys_config = hpactor::Config{};
    auto result =
        hpactor::python::PythonNativeSystem::create(sys_config, rt_config);
    if (!result.ok()) {
        PyErr_SetString(PyExc_RuntimeError, "failed to create native system");
        throw py::error_already_set();
    }
    native_ = std::move(result.value());
}

NativeSystemObject::~NativeSystemObject() = default;

// ═══════════════════════════════════════════════════════════════════════
// Lifecycle
// ═══════════════════════════════════════════════════════════════════════

bool NativeSystemObject::start() noexcept {
    return guard([&] {
        auto result = native_->start();
        if (!result.ok()) {
            PyErr_SetString(PyExc_RuntimeError, "native system start failed");
            throw py::error_already_set();
        }
        return true;
    });
}

bool NativeSystemObject::stop() noexcept {
    return guard([&] {
        auto result = native_->stop();
        (void)result;
        return true;
    });
}

bool NativeSystemObject::begin_draining() noexcept {
    return guard([&] {
        auto result = native_->begin_draining();
        (void)result;
        return true;
    });
}

// ═══════════════════════════════════════════════════════════════════════
// Actor management
// ═══════════════════════════════════════════════════════════════════════

py::object NativeSystemObject::application_origin() noexcept {
    return guard([&]() -> py::object {
        return address_to_tuple(native_->application_origin());
    });
}

py::object NativeSystemObject::spawn_bridge() noexcept {
    return guard([&]() -> py::object {
        auto spawned = native_->spawn_bridge();
        if (!spawned.ok()) {
            PyErr_SetString(PyExc_RuntimeError, "spawn_bridge failed");
            throw py::error_already_set();
        }
        auto addr_tup = address_to_tuple(spawned.value().address);
        return py::make_tuple(addr_tup, spawned.value().generation);
    });
}

bool NativeSystemObject::stop_bridge(py::tuple address) noexcept {
    return guard([&] {
        auto addr = tuple_to_address(address);
        if (!addr.has_value()) {
            PyErr_SetString(PyExc_ValueError, "invalid actor address tuple");
            throw py::error_already_set();
        }
        auto result = native_->stop_bridge(*addr);
        if (!result.ok()) {
            PyErr_SetString(PyExc_RuntimeError, "stop_bridge failed");
            throw py::error_already_set();
        }
        return true;
    });
}

bool NativeSystemObject::register_name(const std::string& name,
                                       py::tuple address) noexcept {
    return guard([&] {
        auto addr = tuple_to_address(address);
        if (!addr.has_value()) {
            PyErr_SetString(PyExc_ValueError, "invalid actor address tuple");
            throw py::error_already_set();
        }
        auto result = native_->register_name(name, *addr);
        if (!result.ok()) {
            PyErr_SetString(PyExc_RuntimeError, "register_name failed");
            throw py::error_already_set();
        }
        return true;
    });
}

py::object NativeSystemObject::resolve_name(const std::string& name) noexcept {
    return guard([&]() -> py::object {
        auto addr = native_->resolve_name(name);
        return address_to_tuple(addr);
    });
}

// ═══════════════════════════════════════════════════════════════════════
// Message passing
// ═══════════════════════════════════════════════════════════════════════

bool NativeSystemObject::submit(py::dict command) noexcept {
    return guard([&] {
        auto cmd = dict_to_command(command);
        if (!cmd.has_value()) {
            PyErr_SetString(PyExc_ValueError, "invalid command dict");
            throw py::error_already_set();
        }
        auto ptr = std::make_shared<const hpactor::python::PythonCommand>(*cmd);
        return native_->submit(ptr);
    });
}

py::list NativeSystemObject::drain_dispatch(uint32_t max_items) noexcept {
    return guard([&] {
        py::list result;
        native_->drain_dispatch(
            max_items, [&](const hpactor::python::PythonDispatchEnvelope& env) {
                result.append(dispatch_to_dict(env));
            });
        return result;
    });
}

py::list NativeSystemObject::drain_completions(uint32_t max_items) noexcept {
    return guard([&] {
        py::list result;
        native_->drain_completions(
            max_items, [&](const hpactor::python::PythonCompletion& comp) {
                result.append(completion_to_dict(comp));
            });
        return result;
    });
}

// ═══════════════════════════════════════════════════════════════════════
// Observability
// ═══════════════════════════════════════════════════════════════════════

py::dict NativeSystemObject::snapshot() noexcept {
    return guard([&] {
        auto snap = native_->snapshot();
        py::dict d;
        d["state"] = static_cast<int>(snap.state);
        d["dispatch_depth"] = snap.queues.dispatch_depth;
        d["command_depth"] = snap.queues.command_depth;
        d["completion_depth"] = snap.queues.completion_depth;
        d["dispatch_rejected"] = snap.queues.dispatch_rejected;
        d["command_rejected"] = snap.queues.command_rejected;
        d["actor_bindings"] = snap.actor_bindings;
        return d;
    });
}

int NativeSystemObject::dispatch_fd() const noexcept {
    return native_ ? native_->dispatch_read_fd() : -1;
}

int NativeSystemObject::completion_fd() const noexcept {
    return native_ ? native_->completion_read_fd() : -1;
}

// ═══════════════════════════════════════════════════════════════════════
// Topology stubs (Phase 1E)
// ═══════════════════════════════════════════════════════════════════════

py::object
NativeSystemObject::prepare_topology(const std::string& /*path*/) noexcept {
    return guard([&]() -> py::object {
        PyErr_SetString(PyExc_NotImplementedError, "topology not yet implemented");
        throw py::error_already_set();
    });
}

bool NativeSystemObject::bind_topology_manifest(py::list /*bindings*/) noexcept {
    return guard([&]() -> bool {
        PyErr_SetString(PyExc_NotImplementedError, "topology not yet implemented");
        throw py::error_already_set();
    });
}

bool NativeSystemObject::start_prepared_topology() noexcept {
    return guard([&]() -> bool {
        PyErr_SetString(PyExc_NotImplementedError, "topology not yet implemented");
        throw py::error_already_set();
    });
}

bool NativeSystemObject::complete_topology_actor(py::dict /*outcome*/) noexcept {
    return guard([&]() -> bool {
        PyErr_SetString(PyExc_NotImplementedError, "topology not yet implemented");
        throw py::error_already_set();
    });
}

bool NativeSystemObject::record_topology_preflight(py::dict /*preflight*/) noexcept {
    return guard([&]() -> bool {
        PyErr_SetString(PyExc_NotImplementedError, "topology not yet implemented");
        throw py::error_already_set();
    });
}

py::object NativeSystemObject::last_topology_error() noexcept {
    return guard([&]() -> py::object { return py::none(); });
}

// ═══════════════════════════════════════════════════════════════════════
// Conversion helpers
// ═══════════════════════════════════════════════════════════════════════

py::tuple
NativeSystemObject::address_to_tuple(const hpactor::ActorAddress& addr) noexcept {
    int family = 0;
    py::bytes packed;
    uint16_t port = 0;

    std::visit(
        [&](const auto& ep) {
            using T = std::decay_t<decltype(ep)>;
            if constexpr (std::is_same_v<T, hpactor::Ipv4Endpoint>) {
                family = 4;
                // addr is in network byte order; pack as 4 big-endian bytes
                uint32_t a = ep.addr;
                uint8_t buf[4] = {
                    static_cast<uint8_t>(a >> 24), static_cast<uint8_t>(a >> 16),
                    static_cast<uint8_t>(a >> 8), static_cast<uint8_t>(a)};
                packed = py::bytes(reinterpret_cast<const char*>(buf), 4);
                port = ep.port();
            } else if constexpr (std::is_same_v<T, hpactor::Ipv6Endpoint>) {
                family = 6;
                packed =
                    py::bytes(reinterpret_cast<const char*>(ep.addr.data()), 16);
                port = ep.port();
            }
        },
        addr.endpoint);

    return py::make_tuple(family, packed, port, addr.type, addr.id.value(),
                          addr.incarnation);
}

std::optional<hpactor::ActorAddress>
NativeSystemObject::tuple_to_address(py::tuple tup) noexcept {
    try {
        if (py::len(tup) != 6)
            return std::nullopt;
        int family = tup[0].cast<int>();
        auto packed_bytes = tup[1].cast<py::bytes>();
        int port = tup[2].cast<int>();
        uint32_t actor_type = tup[3].cast<uint32_t>();
        uint64_t actor_id = tup[4].cast<uint64_t>();
        uint64_t incarnation = tup[5].cast<uint64_t>();

        std::string packed_str = packed_bytes.cast<std::string>();
        hpactor::EndPoint endpoint;
        if (family == 4 && packed_str.size() == 4) {
            const auto* p = reinterpret_cast<const uint8_t*>(packed_str.data());
            uint32_t addr = (static_cast<uint32_t>(p[0]) << 24) |
                            (static_cast<uint32_t>(p[1]) << 16) |
                            (static_cast<uint32_t>(p[2]) << 8) |
                            static_cast<uint32_t>(p[3]);
            endpoint = hpactor::Ipv4Endpoint{
                addr, hpactor::net_to_host_u16(static_cast<uint16_t>(port))};
        } else if (family == 6 && packed_str.size() == 16) {
            std::array<uint8_t, 16> addr;
            std::memcpy(addr.data(), packed_str.data(), 16);
            endpoint = hpactor::Ipv6Endpoint{
                addr, hpactor::net_to_host_u16(static_cast<uint16_t>(port))};
        } else if (family == 0 && packed_str.empty()) {
            // family 0 (local) — use default loopback endpoint
            endpoint = hpactor::Ipv4Endpoint{
                0x7F000001, hpactor::net_to_host_u16(static_cast<uint16_t>(port))};
        } else {
            return std::nullopt;
        }

        return hpactor::ActorAddress(endpoint, hpactor::ActorType{actor_type},
                                     hpactor::ActorId{actor_id}, incarnation);
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        return std::nullopt;
    } catch (const py::cast_error&) {
        PyErr_Clear();
        return std::nullopt;
    }
}

py::dict NativeSystemObject::dispatch_to_dict(
    const hpactor::python::PythonDispatchEnvelope& env) noexcept {
    py::dict d;
    d["kind"] = static_cast<uint8_t>(env.kind);
    d["actor"] = address_to_tuple(env.actor);
    d["generation"] = env.generation;
    d["type_tag"] = static_cast<uint32_t>(env.type_tag);
    d["payload"] = py::bytes(reinterpret_cast<const char*>(env.payload.begin()),
                             env.payload.size());
    d["sender"] = address_to_tuple(env.sender);
    d["message_id"] = env.message_id;
    d["ask_message_id"] = env.ask_message_id;
    d["priority"] = env.priority;
    d["deadline_ns"] = env.deadline_ns;
    d["flags"] = env.flags;
    d["ack_requested"] = env.ack_requested;
    d["sequence"] = env.sequence;
    return d;
}

py::dict NativeSystemObject::completion_to_dict(
    const hpactor::python::PythonCompletion& comp) noexcept {
    py::dict d;
    d["kind"] = static_cast<uint8_t>(comp.kind);
    d["token"] = comp.token;
    d["sequence"] = comp.sequence;
    d["failure_reason"] = static_cast<int>(comp.failure);
    d["failure_source"] = static_cast<int>(comp.source);
    d["actor"] = address_to_tuple(comp.actor);
    d["generation"] = comp.generation;
    d["type_tag"] = static_cast<uint32_t>(comp.type_tag);
    d["payload"] = py::bytes(reinterpret_cast<const char*>(comp.payload.begin()),
                             comp.payload.size());
    d["error_code"] = comp.error_code;
    d["detail"] = py::bytes(comp.detail.data(), comp.detail.size());
    d["schedule_handle"] = comp.schedule_handle;
    d["delivery_status"] = static_cast<int>(comp.delivery_status);
    d["retry_after_ns"] = comp.retry_after_ns;
    return d;
}

std::optional<hpactor::python::PythonCommand>
NativeSystemObject::dict_to_command(py::dict cmd_dict) noexcept {
    try {
        hpactor::python::PythonCommand cmd;
        if (!cmd_dict.contains("kind") || !cmd_dict.contains("token") ||
            !cmd_dict.contains("sequence") ||
            !cmd_dict.contains("generation") || !cmd_dict.contains("origin") ||
            !cmd_dict.contains("target") || !cmd_dict.contains("type_tag"))
            return std::nullopt;

        cmd.kind = static_cast<hpactor::python::PythonCommandKind>(
            cmd_dict["kind"].cast<uint8_t>());
        cmd.token = cmd_dict["token"].cast<uint64_t>();
        cmd.sequence = cmd_dict["sequence"].cast<uint64_t>();
        cmd.generation = cmd_dict["generation"].cast<uint64_t>();

        auto origin = tuple_to_address(cmd_dict["origin"].cast<py::tuple>());
        if (!origin.has_value())
            return std::nullopt;
        cmd.origin = *origin;

        auto target = tuple_to_address(cmd_dict["target"].cast<py::tuple>());
        if (!target.has_value())
            return std::nullopt;
        cmd.target = *target;

        cmd.type_tag = hpactor::TypeTag(cmd_dict["type_tag"].cast<uint32_t>());

        if (cmd_dict.contains("payload")) {
            auto payload_str = cmd_dict["payload"].cast<std::string>();
            cmd.payload = hpactor::StreamBuffer::from_data(
                reinterpret_cast<const uint8_t*>(payload_str.data()),
                payload_str.size());
        }

        if (cmd_dict.contains("reply_to")) {
            auto reply = tuple_to_address(cmd_dict["reply_to"].cast<py::tuple>());
            if (reply.has_value())
                cmd.reply_to = *reply;
        }
        if (cmd_dict.contains("message_id"))
            cmd.message_id = cmd_dict["message_id"].cast<uint64_t>();
        if (cmd_dict.contains("ask_message_id"))
            cmd.ask_message_id = cmd_dict["ask_message_id"].cast<uint64_t>();
        if (cmd_dict.contains("priority"))
            cmd.priority = cmd_dict["priority"].cast<uint8_t>();
        if (cmd_dict.contains("deadline_ns"))
            cmd.deadline_ns = cmd_dict["deadline_ns"].cast<int64_t>();
        if (cmd_dict.contains("flags"))
            cmd.flags = cmd_dict["flags"].cast<uint32_t>();
        if (cmd_dict.contains("delay_ns"))
            cmd.delay_ns = cmd_dict["delay_ns"].cast<uint64_t>();
        if (cmd_dict.contains("schedule_handle"))
            cmd.schedule_handle = cmd_dict["schedule_handle"].cast<uint64_t>();
        if (cmd_dict.contains("error_code"))
            cmd.error_code = cmd_dict["error_code"].cast<uint32_t>();
        if (cmd_dict.contains("detail"))
            cmd.detail = cmd_dict["detail"].cast<std::string>();
        if (cmd_dict.contains("actor_name"))
            cmd.actor_name = cmd_dict["actor_name"].cast<std::string>();
        if (cmd_dict.contains("delivery_mode"))
            cmd.delivery_mode = cmd_dict["delivery_mode"].cast<uint32_t>();
        if (cmd_dict.contains("no_drop"))
            cmd.no_drop = cmd_dict["no_drop"].cast<bool>();
        if (cmd_dict.contains("emit_backpressure"))
            cmd.emit_backpressure = cmd_dict["emit_backpressure"].cast<bool>();

        return cmd;
    } catch (const py::error_already_set&) {
        PyErr_Clear();
        return std::nullopt;
    }
}
