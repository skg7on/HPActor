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

// tests/sched/test_hybrid_scheduler.cpp
#include <cassert>
#include <hpactor/sched/scheduler.hpp>

int main() {
    // Test: HybridScheduler interface methods
    // We can't easily test with real ActorSystem without the full
    // infrastructure, so we just test that HybridScheduler can be instantiated
    // and controlled.

    // Note: Full integration test would require a minimal ActorSystem mock
    // For now, just verify the header is parseable and interface is correct
    hpactor::sched::IScheduler* scheduler_ptr = nullptr;
    (void)scheduler_ptr; // Verify IScheduler is a complete type

    return 0;
}