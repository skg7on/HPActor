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

#include "configured_cpp_provider.hpp"

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/config/actor_factory_registry.hpp>
#include <hpactor/config/topology_model.hpp>

namespace hpactor {

namespace {

// ── Context ────────────────────────────────────────────────────────────────

/// Opaque context carried by the provider port.
struct CppProviderContext final {
    ActorSystem* system{nullptr};
};

// ── matches ────────────────────────────────────────────────────────────────

bool cpp_matches(void* ctx, const ConfiguredActorPlan& plan) noexcept {
    (void)ctx;
    if (plan.provider != ConfiguredActorProviderKind::BuiltinCpp) {
        return false;
    }
    // A BuiltinCpp plan with token 0 is always eligible; the factory
    // lookup in spawn_unpublished will catch an unknown behavior name.
    return plan.provider_token == 0;
}

// ── spawn_unpublished ──────────────────────────────────────────────────────

result<ActorSpawnLease> cpp_spawn_unpublished(
    void* ctx, const config::ActorDef& def,
    const ConfiguredActorPlan& plan) noexcept {
    (void)plan;
    auto* provider_ctx = static_cast<CppProviderContext*>(ctx);
    auto& sys = *provider_ctx->system;

    // Look up the factory by behavior name.
    auto factory =
        config::ActorFactoryRegistry::instance().get_factory(def.behavior);
    if (!factory) {
        return result<ActorSpawnLease>::make(
            error(errors::unknown, "no C++ factory registered for behavior"));
    }

    // Construct the actor (project compiled with -fno-exceptions; if the
    // factory throws, the process terminates — which is the intended
    // behaviour for a statically-registered C++ actor constructor).
    std::shared_ptr<AbstractActor> actor_ptr = factory(nullptr, sys);

    if (!actor_ptr) {
        return result<ActorSpawnLease>::make(
            error(errors::unknown, "C++ actor factory returned null"));
    }

    // Spawn through the existing spawn_configured pipeline.
    Actor spawned = sys.spawn_configured(std::move(actor_ptr), def);
    if (!spawned) {
        return result<ActorSpawnLease>::make(
            error(errors::unknown, "spawn_configured failed"));
    }

    // Return an empty lease — the actor is already fully published via
    // spawn_configured. Rollback is handled through the rollback_actor
    // callback which drains and stops the actor.
    return result<ActorSpawnLease>::make(ActorSpawnLease{});
}

// ── await_ready ────────────────────────────────────────────────────────────

result<void> cpp_await_ready(void* /*ctx*/, ActorId /*actor_id*/,
                              const ConfiguredActorPlan& /*plan*/,
                              std::chrono::milliseconds /*timeout*/) noexcept {
    // C++ actors are fully active after spawn_configured returns.
    // No async handshake is needed.
    return result<void>::make();
}

// ── rollback_actor ─────────────────────────────────────────────────────────

result<void> cpp_rollback_actor(void* ctx, ActorId actor_id,
                                 const ConfiguredActorPlan& /*plan*/) noexcept {
    auto* provider_ctx = static_cast<CppProviderContext*>(ctx);
    auto& sys = *provider_ctx->system;

    // Best-effort drain and stop.
    auto actor = sys.get_actor(actor_id);
    if (!actor) {
        return result<void>::make(); // Already gone — idempotent.
    }

    // Configure immediate drain + drop.
    DrainConfig drain_cfg;
    drain_cfg.policy = DrainPolicy::DropUserMessages;
    drain_cfg.timeout = std::chrono::milliseconds{0};
    sys.set_drain_config(actor_id, drain_cfg);

    // Lifecycle: transition to Failed so the actor stops processing.
    if (auto* lifecycle = actor->as_lifecycle()) {
        lifecycle->transition(LifecycleState::kFailed);
    }

    return result<void>::make();
}

} // namespace

ConfiguredActorProviderPort
make_builtin_cpp_provider_port(ActorSystem& system) noexcept {
    // The context is allocated here and owned by the caller.
    // In practice PythonNativeSystem holds one port and one context
    // for the lifetime of start_prepared_topology().
    auto* ctx = new CppProviderContext{&system};

    ConfiguredActorProviderPort port;
    port.context = ctx;
    port.matches = cpp_matches;
    port.spawn_unpublished = cpp_spawn_unpublished;
    port.await_ready = cpp_await_ready;
    port.rollback_actor = cpp_rollback_actor;
    return port;
}

} // namespace hpactor
