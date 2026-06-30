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

#include "runtime_startup.hpp"

#include "actor_system_impl.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/sched/scheduler.hpp>

namespace hpactor {
namespace {

// ── Stage action helpers ────────────────────────────────────────────────────
//
// Each stage captures a pointer to the component it manages via the void*
// context. The start action initializes/activates the component. The rollback
// action reverses the start if possible.

// ── Stage 1: Scheduler ──────────────────────────────────────────────────────

struct SchedulerStageCtx {
    sched::IScheduler* scheduler;
};

bool start_scheduler(void* ctx) noexcept {
    auto* c = static_cast<SchedulerStageCtx*>(ctx);
    if (!c || !c->scheduler)
        return false;
    c->scheduler->start();
    return true;
}

bool rollback_scheduler(void* ctx) noexcept {
    auto* c = static_cast<SchedulerStageCtx*>(ctx);
    if (!c || !c->scheduler)
        return false;
    c->scheduler->stop();
    return true;
}

void destroy_scheduler_ctx(void* ctx) noexcept {
    delete static_cast<SchedulerStageCtx*>(ctx);
}

// ── Stage 2: Fault controller ───────────────────────────────────────────────

struct FaultCtx {
    fault::FaultController* controller;
};

bool start_fault(void* ctx) noexcept {
    auto* c = static_cast<FaultCtx*>(ctx);
    if (!c || !c->controller)
        return false;
    c->controller->install();
    return true;
}

bool rollback_fault(void* ctx) noexcept {
    auto* c = static_cast<FaultCtx*>(ctx);
    if (!c || !c->controller)
        return false;
    c->controller->remove();
    return true;
}

void destroy_fault_ctx(void* ctx) noexcept {
    delete static_cast<FaultCtx*>(ctx);
}

} // namespace

void register_runtime_startup_stages(RuntimeCoordinator& coord, ActorSystem& system,
                                     bool /*enable_net*/) noexcept {
    auto& impl = *system.impl_;

    // ── Stage 1: Scheduler ──────────────────────────────────────────────────
    {
        auto* ctx = new SchedulerStageCtx{impl.core.scheduler.get()};
        coord.add_stage(RuntimeLifecycleStage{
            .name = "scheduler",
            .start = {.context = ctx, .action = start_scheduler},
            .rollback = {.context = ctx, .action = rollback_scheduler},
            .destroy_context = destroy_scheduler_ctx,
        });
    }

    // ── Stage 2: Fault controller ───────────────────────────────────────────
    {
        auto* ctx = new FaultCtx{&impl.operations.fault_controller};
        coord.add_stage(RuntimeLifecycleStage{
            .name = "fault",
            .start = {.context = ctx, .action = start_fault},
            .rollback = {.context = ctx, .action = rollback_fault},
            .destroy_context = destroy_fault_ctx,
        });
    }
}

} // namespace hpactor
