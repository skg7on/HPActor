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

#include <hpactor/mem/compaction.hpp>
#include <hpactor/mem/slab_cache.hpp>
#include <hpactor/mem/size_class.hpp>

#include <cassert>
#include <iostream>

int main() {
    using namespace hpactor::mem;

    // Test compaction threshold logic
    {
        CompactionManager mgr;

        // Full slab — no compaction needed
        assert(!mgr.should_compact_slab(512, 512)); // 100% utilization

        // 50% utilization — above threshold
        assert(!mgr.should_compact_slab(256, 512));

        // 25% utilization — at threshold
        assert(mgr.should_compact_slab(128, 512));

        // 10% utilization — below threshold
        assert(mgr.should_compact_slab(50, 512));

        // Empty — should compact
        assert(mgr.should_compact_slab(0, 512));

        // Edge: zero total blocks
        assert(!mgr.should_compact_slab(0, 0));
    }

    // Test waste computation
    {
        SlabCache cache(SizeClass::k128B);

        // Allocate some blocks
        for (int i = 0; i < 100; ++i) {
            void* p = cache.allocate(hpactor::ActorId{static_cast<uint64_t>(i)});
            assert(p != nullptr);
        }

        // Free half — creates fragmentation
        for (int i = 0; i < 50; ++i) {
            auto* hdr = hpactor::mem::AllocHeader::from_user_data(
                cache.allocate(hpactor::ActorId{1000}));
            (void)hdr;
        }

        auto waste = CompactionManager::compute_waste(cache);
        // Verify the report is non-negative
        assert(waste.waste_ratio >= 0.0f);
        assert(waste.total_bytes > 0);
        (void)waste;
    }

    // Test compaction interval enforcement
    {
        CompactionConfig cfg;
        cfg.compaction_interval_ms = 60000;
        CompactionManager mgr(cfg);

        // First call: should compact (never compacted before)
        assert(mgr.should_compact());

        // Record compaction
        mgr.record_compaction();

        // Immediately after: should NOT compact (interval not elapsed)
        assert(!mgr.should_compact());
    }

    // Test fragmentation budget config
    {
        CompactionConfig cfg;
        assert(cfg.fragmentation_budget == 0.05f);
        assert(cfg.compaction_threshold == 0.25f);

        CompactionManager mgr(cfg);
        assert(mgr.config().fragmentation_budget == 0.05f);
    }

    // Test SlabCompactionInfo
    {
        SlabCompactionInfo info;
        info.total_blocks = 1000;
        info.compaction_threshold_blocks = 250; // 25%

        assert(!info.should_compact(500)); // 50% — no
        assert(info.should_compact(250));  // 25% — yes
        assert(info.should_compact(100));  // 10% — yes

        float util = info.utilization(500);
        assert(util == 0.5f);
    }

    std::cout << "test_compaction: PASS\n";
    return 0;
}
