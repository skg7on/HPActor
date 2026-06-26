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

#pragma once

#include "caf_bench_config.hpp"
#include "caf_bench_metrics.hpp"
#include "caf_bench_scenarios.hpp"
#include "caf_bench_sweep.hpp"

namespace hpactor::apps::bench_caf {

inline TrialMetrics run_one_trial(const CafBenchConfig& cfg, uint32_t trial_index) {
    switch (cfg.scenario) {
        case ScenarioKind::ActorCreation:
            return run_actor_creation_trial(cfg, trial_index);
        case ScenarioKind::MailboxN1:
            return run_mailbox_n1_trial(cfg, trial_index);
        case ScenarioKind::MixedCase:
            return run_mixed_case_trial(cfg, trial_index);
        case ScenarioKind::TrafficOneToOne:
            return run_one_to_one_trial(cfg, trial_index);
        case ScenarioKind::TrafficOneToN:
            return run_one_to_n_trial(cfg, trial_index);
        case ScenarioKind::TrafficNToNRandom:
            return run_n_to_n_random_trial(cfg, trial_index);
        case ScenarioKind::TrafficRing:
            return run_ring_traffic_trial(cfg, trial_index);
        case ScenarioKind::TrafficPipeline:
            return run_pipeline_trial(cfg, trial_index);
        case ScenarioKind::TrafficZipf:
            return run_zipf_hotspot_trial(cfg, trial_index);
        case ScenarioKind::TrafficBursty:
            return run_bursty_waves_trial(cfg, trial_index);
    }
    return run_actor_creation_trial(cfg, trial_index);
}

inline CafBenchReport run_caf_benchmark(const CafBenchConfig& cfg) {
    CafBenchReport report;
    report.config = cfg;

    auto sweep = expand_sweep(cfg);

    // Run warmup trials on the first sweep entry.
    for (uint32_t i = 0; i < cfg.warmups; ++i) {
        (void)run_one_trial(sweep[0].config, i + 1);
    }

    // Run measured trials for every sweep entry.
    for (const auto& entry : sweep) {
        for (uint32_t i = 0; i < cfg.trials; ++i) {
            report.trials.push_back(run_one_trial(entry.config, i + 1));
        }
    }
    return report;
}

} // namespace hpactor::apps::bench_caf
