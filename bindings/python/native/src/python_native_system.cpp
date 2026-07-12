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
#include <hpactor/python/python_topology_preparer.hpp>
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

result<std::vector<PythonTopologyDescriptor>>
PythonNativeSystem::prepare_topology(std::string_view path) noexcept {
    auto plan_result = PythonTopologyPreparer::parse(path);
    if (!plan_result.has_value()) {
        return result<std::vector<PythonTopologyDescriptor>>::make(
            plan_result.error());
    }
    auto plan = std::move(plan_result.value());

    // Build descriptors for Python actors.
    std::vector<PythonTopologyDescriptor> descriptors;
    const auto& actors = plan->actors();
    for (const auto& spec : actors) {
        if (spec.kind != ConfiguredActorKind::Python)
            continue;
        const auto& def = plan->model().actors[spec.topology_index];
        PythonTopologyDescriptor desc;
        desc.topology_index = spec.topology_index;
        desc.actor_id = def.id;
        desc.behavior = def.behavior;
        desc.args_fingerprint = spec.args_fingerprint;
        if (spec.python.has_value()) {
            desc.module = spec.python->module;
            desc.qualname = spec.python->qualname;
        }
        // Collect sorted args.
        for (const auto& [k, v] : def.args) {
            desc.args.emplace_back(k, v);
        }
        std::sort(desc.args.begin(), desc.args.end());
        descriptors.push_back(std::move(desc));
    }

    parsed_plan_ = std::move(plan);
    return result<std::vector<PythonTopologyDescriptor>>::make(
        std::move(descriptors));
}

result<uint64_t>
PythonNativeSystem::bind_topology_manifest(
    std::span<const FactoryTokenBinding> bindings,
    uint64_t policy_fingerprint) noexcept {
    if (!parsed_plan_) {
        return result<uint64_t>::make(
            error(errors::invalid_argument, "no parsed topology plan"));
    }

    auto prepared_result =
        parsed_plan_->bind_manifest(bindings, policy_fingerprint);
    if (!prepared_result.has_value()) {
        return result<uint64_t>::make(prepared_result.error());
    }

    auto prepared = std::move(prepared_result.value());
    uint64_t fp = prepared->effective_fingerprint();
    prepared_ = std::move(prepared);

    // Create topology provider if Python actors exist.
    bool has_python = false;
    for (const auto& spec : prepared_->actors()) {
        if (spec.kind == ConfiguredActorKind::Python) {
            has_python = true;
            break;
        }
    }
    if (has_python && runtime_) {
        size_t max_actors = runtime_->config().max_actor_bindings;
        topology_provider_ = std::make_unique<PythonTopologyProvider>(
            *runtime_, *this, max_actors);
    }

    return result<uint64_t>::make(std::move(fp));
}

result<void> PythonNativeSystem::start_prepared_topology() noexcept {
    if (!prepared_) {
        return result<void>::make(
            error(errors::invalid_argument, "no prepared topology"));
    }
    if (!topology_provider_) {
        return result<void>::make(
            error(errors::invalid_argument, "no topology provider"));
    }
    if (!runtime_ || !system_) {
        return result<void>::make(
            error(errors::unknown, "system not started"));
    }

    const auto& model = prepared_->model();
    const auto actor_specs = prepared_->actors();
    auto timeout = std::chrono::milliseconds(
        runtime_->config().topology_start_timeout_ms);

    // Spawn and track each Python actor.
    struct SpawnRecord {
        ActorAddress bridge_addr;
        uint64_t factory_token;
    };
    std::vector<SpawnRecord> spawned;

    for (size_t i = 0; i < actor_specs.size(); ++i) {
        const auto& spec = actor_specs[i];
        if (spec.kind != ConfiguredActorKind::Python)
            continue; // C++ actors handled via C++ provider (future)

        const auto& def = model.actors[spec.topology_index];

        // Reserve a ready-table slot.
        auto reserve = topology_provider_->ready_table().reserve(
            spec.factory_token);
        if (!reserve.ok()) {
            last_topology_error_ = PythonTopologyErrorInfo{
                PythonTopologyPhase::ActorStart, def.id, def.behavior,
                static_cast<uint32_t>(reserve.error().code()), ""};
            goto rollback;
        }

        // Spawn a bridge actor.
        auto bridge = spawn_bridge();
        if (!bridge.ok()) {
            last_topology_error_ = PythonTopologyErrorInfo{
                PythonTopologyPhase::ActorStart, def.id, def.behavior,
                static_cast<uint32_t>(bridge.error().code()), ""};
            goto rollback;
        }

        // Push TopologyInstall dispatch to the Python runtime.
        auto dispatch = std::make_shared<PythonDispatchEnvelope>();
        dispatch->kind = PythonDispatchKind::TopologyInstall;
        dispatch->actor = bridge.value().address;
        dispatch->generation = bridge.value().generation;
        dispatch->topology_index = spec.topology_index;
        dispatch->factory_token = spec.factory_token;
        dispatch->args_fingerprint = spec.args_fingerprint;

        if (!runtime_->try_push_dispatch(dispatch)) {
            (void)stop_bridge(bridge.value().address);
            last_topology_error_ = PythonTopologyErrorInfo{
                PythonTopologyPhase::ActorStart, def.id, def.behavior,
                static_cast<uint32_t>(
                    static_cast<int>(errors::mailbox_full)), ""};
            goto rollback;
        }

        spawned.push_back(
            {bridge.value().address, spec.factory_token});
    }

    // Await readiness for each actor.
    for (const auto& entry : spawned) {
        auto ready = topology_provider_->ready_table().wait_ready(
            entry.factory_token, timeout);
        if (!ready.ok()) {
            for (const auto& spec : actor_specs) {
                if (spec.factory_token == entry.factory_token) {
                    last_topology_error_ = PythonTopologyErrorInfo{
                        PythonTopologyPhase::ActorStart,
                        model.actors[spec.topology_index].id,
                        model.actors[spec.topology_index].behavior,
                        static_cast<uint32_t>(ready.error().code()),
                        ""};
                    break;
                }
            }
            goto rollback;
        }
    }

    return result<void>::make();

rollback:
    for (auto it = spawned.rbegin(); it != spawned.rend(); ++it) {
        (void)stop_bridge(it->bridge_addr);
    }
    return result<void>::make(
        error(errors::unknown, "topology bootstrap failed"));
}

result<void> PythonNativeSystem::complete_topology_actor(
    uint64_t factory_token, uint64_t system_generation,
    uint64_t actor_generation, uint8_t outcome,
    uint32_t error_code, std::string_view detail) noexcept {
    if (!topology_provider_) {
        return result<void>::make(
            error(errors::invalid_argument, "no topology provider"));
    }

    auto topo_outcome = static_cast<TopologyActorOutcome>(outcome);
    return topology_provider_->ready_table().complete(
        factory_token, system_generation, actor_generation,
        topo_outcome, error_code, detail);
}

PythonTopologyProvider* PythonNativeSystem::topology_provider() noexcept {
    return topology_provider_.get();
}

void PythonNativeSystem::record_topology_preflight(uint8_t phase,
                                                    bool success) noexcept {
    (void)phase; (void)success;
}

PythonTopologyErrorInfo
PythonNativeSystem::last_topology_error() const noexcept {
    return last_topology_error_;
}

} // namespace hpactor::python
