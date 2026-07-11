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

#include <hpactor/python/python_native_system.hpp>

#include <hpactor/actor/lifecycle/drain_policy.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/python/python_bridge_actor.hpp>
#include <hpactor/python/python_command_router.hpp>
#include <hpactor/python/python_topology_provider.hpp>
#include <hpactor/python/python_gateway_actor.hpp>
#include <hpactor/python/python_gateway_wake_adapter.hpp>

namespace hpactor::python {

result<std::unique_ptr<PythonNativeSystem>>
PythonNativeSystem::create(Config system_config,
                           PythonRuntimeConfig python_config) noexcept {
    if (python_config.validate() != PythonConfigError::None) {
        return result<std::unique_ptr<PythonNativeSystem>>::make(
            error(errors::invalid_argument, "invalid python runtime config"));
    }

    auto native = std::unique_ptr<PythonNativeSystem>(new PythonNativeSystem(
        std::move(system_config), std::move(python_config)));
    if (!native->system_) {
        return result<std::unique_ptr<PythonNativeSystem>>::make(
            error(errors::unknown, "failed to create actor system"));
    }
    if (!native->runtime_ || !native->router_) {
        return result<std::unique_ptr<PythonNativeSystem>>::make(
            error(errors::unknown, "failed to create python runtime"));
    }

    return result<std::unique_ptr<PythonNativeSystem>>::make(std::move(native));
}

PythonNativeSystem::PythonNativeSystem(Config system_config,
                                       PythonRuntimeConfig python_config) noexcept {
    system_config.enable_network = false;
    system_ = std::make_unique<ActorSystem>(std::move(system_config));

    auto runtime_created = PythonRuntime::create(std::move(python_config));
    if (!runtime_created.ok())
        return;
    runtime_ = std::move(runtime_created.value());

    router_ = std::make_unique<PythonCommandRouter>(*system_, *runtime_);
}

PythonNativeSystem::~PythonNativeSystem() {
    (void)stop();
}

result<void> PythonNativeSystem::start() noexcept {
    if (!system_ || !runtime_ || !router_)
        return result<void>::make(error(errors::unknown, "not created"));

    // Spawn the gateway actor.
    gateway_ = system_->spawn<PythonGatewayActor>(*runtime_, router_->port());
    if (!gateway_) {
        return result<void>::make(error(errors::unknown, "failed to spawn gateway"));
    }

    // Create the wake adapter.
    wake_adapter_ =
        std::make_unique<PythonGatewayWakeAdapter>(*system_, gateway_.address());

    // Start the runtime.
    auto start_result = runtime_->start(wake_adapter_->port());
    if (!start_result.ok())
        return start_result;

    // Spawn the hidden application bridge.
    auto lease_opt = runtime_->reserve_actor();
    if (!lease_opt.has_value()) {
        return result<void>::make(
            error(errors::mailbox_full, "no actor leases available"));
    }
    application_bridge_ = system_->spawn<PythonBridgeActor>(
        *runtime_, std::move(*lease_opt), reliability_, PythonSupervisionConfig{});
    if (!application_bridge_) {
        return result<void>::make(
            error(errors::unknown, "failed to spawn application bridge"));
    }
    // PythonBridgeActor stores its generation on the lease; retrieve
    // from the abstract actor via the known type.
    auto ptr = application_bridge_.get();
    if (ptr && ptr->type_name() == PythonBridgeActor::kActorTypeName) {
        app_generation_ = static_cast<PythonBridgeActor*>(ptr.get())->generation();
    }

    return result<void>::make();
}

result<void> PythonNativeSystem::begin_draining() noexcept {
    if (runtime_)
        runtime_->begin_draining();
    return result<void>::make();
}

result<void> PythonNativeSystem::stop() noexcept {
    if (runtime_)
        runtime_->begin_draining();

    // shutdown() is idempotent
    if (system_)
        (void)system_->shutdown();

    application_bridge_ = Actor{};
    gateway_ = Actor{};
    wake_adapter_.reset();
    router_.reset();
    name_registry_.clear();

    return result<void>::make();
}

result<PythonSpawnedActor> PythonNativeSystem::spawn_bridge() noexcept {
    if (!runtime_ || !system_)
        return result<PythonSpawnedActor>::make(
            error(errors::unknown, "not started"));

    auto lease_opt = runtime_->reserve_actor();
    if (!lease_opt.has_value()) {
        return result<PythonSpawnedActor>::make(
            error(errors::mailbox_full, "no actor leases available"));
    }

    auto gen = lease_opt->generation();
    auto bridge = system_->spawn<PythonBridgeActor>(
        *runtime_, std::move(*lease_opt), reliability_, PythonSupervisionConfig{});
    if (!bridge) {
        return result<PythonSpawnedActor>::make(
            error(errors::unknown, "spawn failed"));
    }

    PythonSpawnedActor spawned{};
    spawned.address = bridge.address();
    spawned.generation = gen;
    return result<PythonSpawnedActor>::make(std::move(spawned));
}

result<void> PythonNativeSystem::stop_bridge(ActorAddress actor) noexcept {
    if (!system_)
        return result<void>::make(error(errors::unknown, "not started"));

    auto ptr = system_->get_actor(actor.id);
    if (ptr) {
        system_->set_drain_config(actor.id,
                                  DrainConfig{DrainPolicy::DropUserMessages,
                                              std::chrono::milliseconds{0}});
    }
    return result<void>::make();
}

result<void> PythonNativeSystem::register_name(std::string_view name,
                                               ActorAddress actor) noexcept {
    if (name.empty() || name.size() > 255)
        return result<void>::make(
            error(errors::invalid_argument, "name too long or empty"));
    name_registry_[std::string(name)] = actor;
    return result<void>::make();
}

ActorAddress PythonNativeSystem::resolve_name(std::string_view name) const noexcept {
    auto it = name_registry_.find(std::string(name));
    if (it != name_registry_.end())
        return it->second;
    return ActorAddress{};
}

ActorAddress PythonNativeSystem::application_origin() const noexcept {
    if (application_bridge_)
        return application_bridge_.address();
    return ActorAddress{};
}

uint64_t PythonNativeSystem::application_generation() const noexcept {
    return app_generation_;
}

bool PythonNativeSystem::submit(const PythonCommandPtr& command) noexcept {
    if (!runtime_ || !command)
        return false;
    return runtime_->try_push_command(command);
}

int PythonNativeSystem::dispatch_read_fd() const noexcept {
    if (runtime_)
        return runtime_->dispatch_read_fd();
    return -1;
}

int PythonNativeSystem::completion_read_fd() const noexcept {
    if (runtime_)
        return runtime_->completion_read_fd();
    return -1;
}

PythonRuntimeSnapshot PythonNativeSystem::snapshot() const noexcept {
    if (runtime_)
        return runtime_->snapshot();
    return PythonRuntimeSnapshot{};
}

PythonTopologyProvider* PythonNativeSystem::topology_provider() noexcept {
    return topology_provider_.get();
}

void PythonNativeSystem::record_topology_preflight(uint8_t phase,
                                                    bool success) noexcept {
    (void)phase; (void)success;
    // TODO: wire into observability counters.
}

PythonTopologyErrorInfo
PythonNativeSystem::last_topology_error() const noexcept {
    return last_topology_error_;
}

} // namespace hpactor::python
