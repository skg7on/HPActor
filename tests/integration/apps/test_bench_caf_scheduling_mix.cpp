// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_scenarios.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(SchedulingMix, SmokeCompletesAllWorkloads) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::SchedulingMix;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;

    auto metrics = bench_caf::run_scheduling_mix_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_GT(metrics.actors_created, 0u);
    EXPECT_GT(metrics.cpu_tasks_completed, 0u);
    EXPECT_GT(metrics.token_hops, 0u);
}
