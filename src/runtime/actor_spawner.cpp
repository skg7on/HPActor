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

#include <hpactor/runtime/actor_spawner.hpp>

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/lifecycle/lifecycle_actor.hpp>
#include <hpactor/actor/local_actor.hpp>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/sched/scheduler.hpp>

namespace hpactor {

namespace {
constexpr ActorId kFirstReservedId{0xFFFF0000};
constexpr ActorId kLastReservedId{0xFFFFFFFF};

bool is_reserved_id(ActorId id) {
    return id.value() >= kFirstReservedId.value() &&
           id.value() <= kLastReservedId.value();
}
} // namespace

ActorSpawner::ActorSpawner(Dependencies dependencies) noexcept
    : deps_(std::move(dependencies)) {}

result<Actor> ActorSpawner::adopt(std::shared_ptr<AbstractActor> actor,
                                  const SpawnSpec& spec) noexcept {
    // ── Validate ──────────────────────────────────────────────────────
    if (!actor) {
        return result<Actor>::make(error(errors::invalid_argument, "null actor"));
    }

    // Check reserved-id constraints
    ActorId id;
    if (spec.reserved_id.has_value()) {
        if (!actor->is_system_actor()) {
            return result<Actor>::make(error(
                errors::invalid_argument, "reserved id requires system actor"));
        }
        if (!is_reserved_id(*spec.reserved_id)) {
            return result<Actor>::make(error(errors::invalid_argument,
                                             "reserved id outside system range"));
        }
        id = *spec.reserved_id;
    } else {
        id = deps_.directory.allocate_id();
        // Guard: automatic allocation must never return a reserved id
        while (is_reserved_id(id)) {
            id = deps_.directory.allocate_id();
        }
    }

    // ── Assign address and type ────────────────────────────────────────
    const ActorType type = spec.actor_type_override.has_value()
                               ? *spec.actor_type_override
                               : actor->type();
    actor->set_address(ActorAddress(deps_.endpoint, type, id, 0));
    actor->set_type_name(std::string{spec.type_name});

    // ── Verify context binding ─────────────────────────────────────────
    auto actor_ctx = std::make_shared<ActorContext>(Actor(actor), &deps_.facade);
    if (!actor->bind_context(actor_ctx.get())) {
        return result<Actor>::make(
            error(errors::invalid_argument, "actor cannot bind local context"));
    }

    actor->set_scheduler(&deps_.scheduler);

    // ── Build mailbox (branch on backend kind) ─────────────────────────
    ActorDirectoryEntry entry;
    entry.actor = Actor(actor);
    entry.instance = actor;
    entry.context = actor_ctx;
    entry.mailbox_kind = actor->mailbox_kind();

    if (entry.mailbox_kind == mailbox::MailboxKind::FixedDisruptor) {
        auto binding = actor->create_fixed_mailbox();
        if (!binding.valid()) {
            return result<Actor>::make(error(errors::invalid_argument,
                                             "fixed mailbox binding is invalid"));
        }
        entry.fixed_mailbox = binding;
        // entry.mailbox remains nullptr for fixed actors.
    } else {
        auto mailbox_ptr =
            std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
                id, &deps_.scheduler, spec.mailbox);
        auto* mbox = mailbox_ptr.get();
        actor->set_mailbox(mbox);

        if (deps_.metrics) [[unlikely]] {
            mbox->set_metrics_ring_buffer(deps_.metrics);
            actor->set_metrics_ring_buffer(deps_.metrics);
        }
        if (deps_.logger) [[unlikely]] {
            mbox->set_logger(deps_.logger);
            actor->set_logger(deps_.logger);
        }
        entry.mailbox = mailbox_ptr;
    }

    // ── Apply quarantine ──────────────────────────────────────────────
    if (spec.quarantine.has_value()) {
        if (auto* eba = actor->is_event_based_actor()
                            ? static_cast<EventBasedActor*>(actor.get())
                            : nullptr) {
            eba->configure_quarantine(*spec.quarantine);
        }
    }

    auto status = deps_.directory.publish(std::move(entry), spec.registered_name);
    if (status != ActorDirectory::PublishStatus::Published) {
        return result<Actor>::make(error(
            errors::invalid_argument, status == ActorDirectory::PublishStatus::DuplicateName
                                          ? "duplicate actor name"
                                          : "duplicate actor id"));
    }

    // ── Fault injection after publication ─────────────────────────────
    FAULT_INJECT("hpactor.actor.spawn.after_publish.fail") {
        rollback_publication(id, *actor);
        return result<Actor>::make(error(errors::unknown, "injected spawn failure"));
    }

    // ── Activate ──────────────────────────────────────────────────────
    actor->activate_after_spawn();

    if (auto* lifecycle = actor->as_lifecycle()) {
        lifecycle->transition(LifecycleState::kActive);
    }

    // ── Register dispatch ─────────────────────────────────────────────
    switch (spec.dispatch_policy) {
        case sched::DispatchPolicy::Cooperative:
            deps_.scheduler.notify_ready(id, 0, INT64_MAX);
            break;
        case sched::DispatchPolicy::DedicatedThread:
            deps_.scheduler.register_dedicated_thread(
                id, spec.dispatch_hints.cpu_affinity);
            break;
        case sched::DispatchPolicy::DedicatedPool:
            deps_.scheduler.register_dedicated_pool(id, spec.dispatch_hints.pool_size);
            break;
    }

    // ── Emit success telemetry ────────────────────────────────────────
    HPACTOR_LOG_INFO(log::LogCategory::kActor, id,
                     static_cast<uint32_t>(log::LogEventId::kActorSpawned),
                     "actor spawned",
                     log::field_lit("type", actor->type_name().data()));

    if (deps_.metrics) [[unlikely]] {
        metrics::MetricEvent evt{};
        evt.actor_id = id;
        evt.event_type = metrics::MetricEventType::kActorSpawned;
        evt.value_hi = 1;
        deps_.metrics->try_push(evt);
    }

    return result<Actor>::make(Actor{std::move(actor)});
}

void ActorSpawner::rollback_publication(ActorId id, AbstractActor& /*actor*/) noexcept {
    deps_.directory.erase(id);
}

} // namespace hpactor
