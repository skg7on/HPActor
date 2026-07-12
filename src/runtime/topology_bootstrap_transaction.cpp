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

#include <hpactor/runtime/topology_bootstrap_transaction.hpp>

#include <hpactor/actor/system/actor_directory.hpp>
#include <hpactor/config/topology_model.hpp>

namespace hpactor {

namespace {

constexpr uint32_t kRollbackNameBit = 1u << 0;
constexpr uint32_t kRollbackProviderBit = 1u << 1;
constexpr uint32_t kRollbackLeaseBit = 1u << 2;

struct JournalEntry {
    ActorSpawnLease lease;
    std::string name;
    ActorId actor_id;
    size_t topology_index;
    bool spawned{false};
};

const ConfiguredActorProviderPort*
choose_provider(const ConfiguredActorPlan& plan,
                const ConfiguredActorProviderPort& cpp,
                const ConfiguredActorProviderPort& ext) {
    if (plan.provider == ConfiguredActorProviderKind::External) {
        return ext.matches != nullptr ? &ext : nullptr;
    }
    return cpp.matches != nullptr ? &cpp : nullptr;
}

} // namespace

result<TopologyBootstrapResult>
TopologyBootstrapTransaction::execute(
    const config::TopologyModel& model,
    std::span<const ConfiguredActorPlan> specs,
    uint64_t effective_fp,
    const ConfiguredActorProviderPort& cpp_provider,
    const ConfiguredActorProviderPort& external_provider,
    ActorDirectory& directory,
    std::chrono::milliseconds actor_start_timeout) noexcept {

    if (specs.size() != model.actors.size()) {
        return result<TopologyBootstrapResult>::make(
            error(errors::invalid_argument, "spec count != actor count"));
    }

    TopologyBootstrapResult outcome;
    outcome.fingerprint = effective_fp;
    outcome.actor_count = static_cast<uint32_t>(specs.size());

    // Phase 1: Pre-allocate journal.
    std::vector<JournalEntry> journal;
    journal.reserve(specs.size());

    // Phase 2: Spawn each actor in model order.
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto& spec = specs[i];
        const auto& def = model.actors[spec.topology_index];

        const auto* provider =
            choose_provider(spec, cpp_provider, external_provider);
        if (!provider) {
            goto rollback;
        }

        JournalEntry entry;
        entry.name = def.id;
        entry.topology_index = spec.topology_index;

        auto spawn_result = provider->spawn_unpublished(
            provider->context, def, spec);
        if (!spawn_result.ok()) {
            goto rollback;
        }

        entry.lease = std::move(spawn_result.value());
        entry.actor_id = entry.lease.actor().id();
        entry.spawned = true;

        auto ready_result = provider->await_ready(
            provider->context, entry.actor_id, spec, actor_start_timeout);
        if (!ready_result.ok()) {
            journal.push_back(std::move(entry));
            goto rollback;
        }

        journal.push_back(std::move(entry));
    }

    // Phase 3: Atomic name registration.
    {
        std::vector<ActorDirectory::NamedActor> names;
        names.reserve(journal.size());
        for (const auto& j : journal) {
            if (!j.name.empty()) {
                names.push_back({j.name, j.actor_id});
            }
        }
        if (!names.empty() && !directory.register_names_atomically(names)) {
            outcome.rollback_error_bits |= kRollbackNameBit;
            goto rollback;
        }
    }

    // Phase 4: Commit all leases.
    for (auto& j : journal) {
        (void)j.lease.commit();
    }

    return result<TopologyBootstrapResult>::make(std::move(outcome));

rollback:
    // Reverse-order rollback using topology_index stored in journal.
    for (auto it = journal.rbegin(); it != journal.rend(); ++it) {
        if (!it->spawned)
            continue;

        // Find the matching spec by topology_index stored in the journal.
        const ConfiguredActorPlan* spec_ptr = nullptr;
        for (const auto& s : specs) {
            if (s.topology_index == it->topology_index) {
                spec_ptr = &s;
                break;
            }
        }

        if (spec_ptr) {
            const auto* provider =
                choose_provider(*spec_ptr, cpp_provider, external_provider);
            if (provider) {
                auto rb = provider->rollback_actor(
                    provider->context, it->actor_id, *spec_ptr);
                if (!rb.ok()) {
                    outcome.rollback_error_bits |= kRollbackProviderBit;
                }
            }
        }

        auto lease_rb = it->lease.rollback();
        if (!lease_rb.ok()) {
            outcome.rollback_error_bits |= kRollbackLeaseBit;
        }
    }

    return result<TopologyBootstrapResult>::make(
        error(errors::unknown, "topology bootstrap failed"));
}

} // namespace hpactor
