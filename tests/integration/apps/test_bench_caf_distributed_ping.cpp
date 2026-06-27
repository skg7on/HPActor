// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_scenarios.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(DistributedPing, LoopbackSmokeCompletes) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::DistributedPing;
    cfg.preset = bench_caf::PresetKind::Smoke;

    auto metrics = bench_caf::run_distributed_ping_trial(cfg, 1);
    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.total_sent, metrics.total_received);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}
