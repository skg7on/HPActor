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

#include <string>
#include <vector>

namespace hpactor::apps::bench_caf {

struct SweepEntry {
    CafBenchConfig config;
    std::string label;
};

inline std::vector<SweepEntry> expand_sweep(const CafBenchConfig& base_cfg) {
    // Smoke and Stress pass through unchanged.
    if (base_cfg.preset == PresetKind::Smoke ||
        base_cfg.preset == PresetKind::Stress) {
        return {{base_cfg, preset_name(base_cfg.preset)}};
    }

    // Nightly and PaperScale sweep message sizes for mailbox-n1.
    if (base_cfg.scenario == ScenarioKind::MailboxN1) {
        // Nightly: 5 sizes (0–1KB). 4KB+ is paper-scale only;
        // nightly volumes (32 senders × 100K) would OOM with large payloads.
        constexpr uint32_t nightly_sizes[] = {0, 16, 64, 256, 1024};
        // PaperScale: 8 sizes (0–64KB).
        constexpr uint32_t all_sizes[] = {0,    16,   64,    256,
                                          1024, 4096, 16384, 65536};
        constexpr size_t kNightlyCount =
            sizeof(nightly_sizes) / sizeof(nightly_sizes[0]);
        constexpr size_t kAllCount = sizeof(all_sizes) / sizeof(all_sizes[0]);
        const uint32_t* sizes = all_sizes;
        size_t count = kAllCount;
        if (base_cfg.preset == PresetKind::Nightly) {
            sizes = nightly_sizes;
            count = kNightlyCount;
        }

        std::vector<SweepEntry> entries;
        entries.reserve(count);
        for (size_t i = 0; i < count; ++i) {
            SweepEntry entry;
            entry.config = base_cfg;
            entry.config.message_size_bytes = sizes[i];
            entry.config.message_shape = sizes[i] == 0 ? MessageShape::HeaderOnly
                                                       : MessageShape::FixedBytes;
            entry.label = "size=" + std::to_string(sizes[i]);
            entries.push_back(std::move(entry));
        }
        return entries;
    }

    // Other scenarios: pass through.
    return {{base_cfg, scenario_name(base_cfg.scenario)}};
}

} // namespace hpactor::apps::bench_caf
