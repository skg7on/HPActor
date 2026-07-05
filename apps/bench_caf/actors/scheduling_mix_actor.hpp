// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");
#pragma once

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/system/actor_system.hpp>

#include "../caf_bench_config.hpp"
#include "../messages.hpp"
#include "mixed_case_actor.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

inline constexpr TypeTag SchedMixTaskTag{0x0001030A};
inline constexpr TypeTag SchedMixDoneTag{0x0001030B};

struct SchedMixDimensions {
    uint32_t waves = 2;
    uint32_t tree_depth = 6;
    uint32_t pool_size = 4;
    uint32_t ring_size = 8;
    uint32_t ring_messages = 100;
};

inline SchedMixDimensions sched_mix_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
        case PresetKind::Smoke:
            return {2, 6, 4, 8, 100};
        case PresetKind::Nightly:
            return {4, 10, 8, 16, 1000};
        case PresetKind::PaperScale:
            return {8, 14, 16, 32, 10000};
        case PresetKind::Stress:
            return {16, 18, 32, 64, 100000};
    }
    return {2, 6, 4, 8, 100};
}

struct SchedMixCounters {
    std::atomic<uint64_t> actors_created{0};
    std::atomic<uint64_t> cpu_tasks_completed{0};
    std::atomic<uint64_t> token_hops{0};
    std::atomic<uint64_t> rings_completed{0};
    std::atomic<uint64_t> waves_done{0};
};

// Concurrent workloads: spawn bursts, CPU tasks, and ring messages.
// Each wave spawns a small creation tree (depth=tree_depth), fires a CPU
// task, and starts a token ring. All three run simultaneously.

} // namespace hpactor::apps::bench_caf
