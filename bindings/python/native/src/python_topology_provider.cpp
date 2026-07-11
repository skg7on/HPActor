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

#include <hpactor/python/python_topology_provider.hpp>

#include <hpactor/config/topology_model.hpp>
#include <hpactor/python/python_native_system.hpp>
#include <hpactor/python/python_runtime.hpp>

namespace hpactor::python {

// ── PythonTopologyReadyTable ────────────────────────────────────────────────

PythonTopologyReadyTable::PythonTopologyReadyTable(size_t max_entries) noexcept
    : max_entries_(max_entries) {}

PythonTopologyReadyTable::~PythonTopologyReadyTable() = default;

result<void>
PythonTopologyReadyTable::reserve(uint64_t factory_token) noexcept {
    std::lock_guard<std::mutex> lock(table_mutex_);
    if (entries_.size() >= max_entries_) {
        return result<void>::make(
            error(errors::mailbox_full, "topology ready table full"));
    }
    // Insert entry if not already present. Use operator[] to default-construct
    // Slot in place (Slot contains non-movable mutex/CV).
    if (entries_.find(factory_token) == entries_.end()) {
        entries_[factory_token]; // default-constructs Slot
    }
    return result<void>::make();
}

size_t PythonTopologyReadyTable::size() const noexcept {
    std::lock_guard<std::mutex> lock(table_mutex_);
    return entries_.size();
}

result<void>
PythonTopologyReadyTable::wait_ready(
    uint64_t factory_token, std::chrono::milliseconds timeout) noexcept {
    Slot* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(table_mutex_);
        auto it = entries_.find(factory_token);
        if (it == entries_.end()) {
            return result<void>::make(
                error(errors::unknown, "factory token not reserved"));
        }
        entry = &it->second;
    }

    std::unique_lock<std::mutex> lock(entry->mutex);
    if (!entry->cv.wait_for(lock, timeout, [&] { return entry->ready; })) {
        return result<void>::make(
            error(errors::timeout, "topology actor startup timed out"));
    }

    if (entry->outcome != TopologyActorOutcome::Ready) {
        return result<void>::make(
            error(errors::unknown, "topology actor startup failed"));
    }

    return result<void>::make();
}

result<void>
PythonTopologyReadyTable::complete(uint64_t factory_token,
                                    uint64_t system_generation,
                                    uint64_t actor_generation,
                                    TopologyActorOutcome outcome,
                                    uint32_t error_code,
                                    std::string_view detail) noexcept {
    (void)system_generation; (void)actor_generation;
    Slot* entry = nullptr;
    {
        std::lock_guard<std::mutex> lock(table_mutex_);
        auto it = entries_.find(factory_token);
        if (it == entries_.end()) {
            return result<void>::make(
                error(errors::unknown, "factory token not reserved"));
        }
        entry = &it->second;
    }

    {
        std::lock_guard<std::mutex> lock(entry->mutex);
        if (entry->ready) {
            // Already completed — idempotent.
            return result<void>::make();
        }
        entry->ready = true;
        entry->outcome = outcome;
        entry->error_code = error_code;
        if (detail.size() > 4096) {
            entry->detail = std::string(detail.substr(0, 4096));
        } else {
            entry->detail = std::string(detail);
        }
    }
    entry->cv.notify_one();
    return result<void>::make();
}

// ── PythonTopologyProvider ──────────────────────────────────────────────────

PythonTopologyProvider::PythonTopologyProvider(
    PythonRuntime& runtime, PythonNativeSystem& native_system,
    size_t max_actors) noexcept
    : runtime_(runtime), native_system_(native_system),
      ready_table_(max_actors) {}

PythonTopologyProvider::~PythonTopologyProvider() = default;

ConfiguredActorProviderPort PythonTopologyProvider::port() noexcept {
    ConfiguredActorProviderPort p;
    p.context = this;
    p.matches = matches;
    p.spawn_unpublished = spawn_unpublished;
    p.await_ready = await_ready;
    p.rollback_actor = rollback_actor;
    return p;
}

PythonTopologyReadyTable& PythonTopologyProvider::ready_table() noexcept {
    return ready_table_;
}

bool PythonTopologyProvider::matches(void* ctx,
                                      const ConfiguredActorPlan& plan) noexcept {
    (void)ctx;
    return plan.provider == ConfiguredActorProviderKind::External &&
           plan.provider_token != 0;
}

result<ActorSpawnLease>
PythonTopologyProvider::spawn_unpublished(
    void* ctx, const config::ActorDef& def,
    const ConfiguredActorPlan& plan) noexcept {
    (void)def;
    auto* self = static_cast<PythonTopologyProvider*>(ctx);

    // Reserve a ready-table slot for this actor.
    auto reserve_result = self->ready_table_.reserve(plan.provider_token);
    if (!reserve_result.ok()) {
        return result<ActorSpawnLease>::make(reserve_result.error());
    }

    // Spawn a bridge actor via PythonNativeSystem.
    auto spawn_result = self->native_system_.spawn_bridge();
    if (!spawn_result.ok()) {
        return result<ActorSpawnLease>::make(spawn_result.error());
    }

    // Build a TopologyInstall dispatch to send to the Python runtime.
    auto dispatch = std::make_shared<PythonDispatchEnvelope>();
    dispatch->kind = PythonDispatchKind::TopologyInstall;
    dispatch->actor = spawn_result.value().address;
    dispatch->generation = spawn_result.value().generation;
    dispatch->topology_index = plan.topology_index;
    dispatch->factory_token = plan.provider_token;
    dispatch->args_fingerprint = plan.args_fingerprint;

    // Enqueue the dispatch to the Python runtime.
    if (!self->runtime_.try_push_dispatch(dispatch)) {
        // Fail: roll back the bridge actor.
        (void)self->native_system_.stop_bridge(spawn_result.value().address);
        return result<ActorSpawnLease>::make(
            error(errors::mailbox_full, "dispatch queue full"));
    }

    // Return an empty lease — the lease tracking for topology actors is
    // handled by the Python bridge lifecycle, not the provider port.
    return result<ActorSpawnLease>::make(ActorSpawnLease{});
}

result<void>
PythonTopologyProvider::await_ready(
    void* ctx, ActorId id, const ConfiguredActorPlan& plan,
    std::chrono::milliseconds timeout) noexcept {
    (void)id;
    auto* self = static_cast<PythonTopologyProvider*>(ctx);
    return self->ready_table_.wait_ready(plan.provider_token, timeout);
}

result<void>
PythonTopologyProvider::rollback_actor(
    void* ctx, ActorId id, const ConfiguredActorPlan& plan) noexcept {
    (void)id;
    auto* self = static_cast<PythonTopologyProvider*>(ctx);

    // Send a TopologyRollback dispatch to the Python runtime.
    auto dispatch = std::make_shared<PythonDispatchEnvelope>();
    dispatch->kind = PythonDispatchKind::TopologyRollback;
    dispatch->factory_token = plan.provider_token;

    // Best-effort enqueue — if the queue is full, the actor is already
    // shutting down and the rollback dispatch is advisory.
    (void)self->runtime_.try_push_dispatch(dispatch);
    return result<void>::make();
}

} // namespace hpactor::python
