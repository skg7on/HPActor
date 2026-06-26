# CAF Performance Benchmark Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the first CAF-style HPActor benchmark deliverable: `apps/bench_caf/18_bench_caf` with `actor-creation`, `mailbox-n1`, and `mixed-case` smoke scenarios plus JSON/CSV output and focused tests.

**Architecture:** Add a standalone benchmark app modeled after `apps/bench_perf/` and `apps/bench_saturate/`, but keep scenario logic split into focused headers under `apps/bench_caf/actors/`. The runner parses a small command-line config, creates an explicit HPActor runtime config, runs one scenario at a time for one or more trials, samples process RSS, and writes stable machine-readable output.

**Tech Stack:** C++20, HPActor `ActorSystem`, `EventBasedActor`, `TypedMessage`, `StreamBuffer`, GTest, CMake/Ninja, existing `hpactor` and `hpactor_lib` targets.

## Global Constraints

- Do not copy CAF implementation code.
- Do not use CAF as a dependency.
- Do not replace `apps/bench_perf/` or `apps/bench_saturate/`.
- Do not require distributed tests in normal CI.
- Do not make benchmark success depend on exact throughput values from one developer machine.
- Do not bypass HPActor delivery semantics by default. Fast-path variants may exist, but each must explicitly state which checks are skipped.
- Every design or implementation write MUST happen in an isolated git worktree.
- Use the worktree-local `build/` directory for configure, build, and test output.
- Production code changes MUST follow RED -> GREEN -> REFACTOR.
- NEVER introduce `dynamic_cast`, `typeid`, exception-based control flow, or public APIs that require RTTI or exceptions.
- Preserve actor boundaries; avoid shared mutable state between actors.
- User-facing sends enter through normal HPActor delivery paths unless a scenario is explicitly marked as a fast-path microbenchmark.
- One serialized consumer per actor mailbox.
- Bounded capacity failures are counted and reported.

---

## File Structure

Create these app files:

```text
apps/bench_caf/
├── CMakeLists.txt
├── 18_bench_caf.cpp
├── README.md
├── caf_bench_config.hpp
├── caf_bench_metrics.hpp
├── caf_bench_output.hpp
├── caf_bench_runner.hpp
├── caf_bench_sampler.hpp
├── caf_bench_scenarios.hpp
├── messages.hpp
└── actors/
    ├── actor_creation_actor.hpp
    ├── mailbox_n1_actor.hpp
    └── mixed_case_actor.hpp
```

Modify these build files:

```text
apps/CMakeLists.txt
tests/unit/apps/CMakeLists.txt
tests/integration/apps/CMakeLists.txt
tests/system/apps/CMakeLists.txt
```

Create these tests:

```text
tests/unit/apps/test_bench_caf_config.cpp
tests/unit/apps/test_bench_caf_metrics.cpp
tests/unit/apps/test_bench_caf_payloads.cpp
tests/integration/apps/test_bench_caf_actor_creation.cpp
tests/integration/apps/test_bench_caf_mailbox_n1.cpp
tests/integration/apps/test_bench_caf_mixed_case.cpp
tests/system/apps/test_bench_caf_smoke.cpp
```

Phase 1 intentionally excludes distributed ping/pong, serialization
microbenchmarks, dispatch matching, Mandelbrot, and the full message
distribution sweep. The runner and output schema must be shaped so those
follow-on scenarios can be added without changing Phase 1 scenario results.

## Shared Interfaces

All tasks use the following public app namespace:

```cpp
namespace hpactor::apps::bench_caf {
// Declarations live here.
}
```

Core types produced in Task 1 and consumed by all scenario tasks:

```cpp
enum class ScenarioKind {
    ActorCreation,
    MailboxN1,
    MixedCase,
};

enum class PresetKind {
    Smoke,
    Nightly,
    PaperScale,
    Stress,
};

enum class OutputFormat {
    Json,
    Csv,
};

enum class MessageShape {
    HeaderOnly,
    FixedBytes,
};

enum class TrafficDistribution {
    NToOne,
};

struct CafBenchConfig {
    ScenarioKind scenario = ScenarioKind::ActorCreation;
    PresetKind preset = PresetKind::Smoke;
    OutputFormat format = OutputFormat::Json;
    MessageShape message_shape = MessageShape::HeaderOnly;
    TrafficDistribution distribution = TrafficDistribution::NToOne;
    uint32_t scheduler_threads = 2;
    uint32_t trials = 1;
    uint32_t warmups = 0;
    uint32_t message_size_bytes = 0;
    uint32_t mailbox_capacity = 4096;
    uint64_t seed = 1;
    uint32_t sample_rss_ms = 50;
    std::string output_path;
};

struct TrialMetrics {
    uint32_t trial = 0;
    bool completed = false;
    uint64_t runtime_ms = 0;
    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    uint64_t total_rejected = 0;
    uint64_t total_dropped = 0;
    uint64_t actors_created = 0;
    uint64_t actors_completed = 0;
    uint64_t rings_completed = 0;
    uint64_t token_hops = 0;
    uint64_t cpu_tasks_completed = 0;
    double throughput_msgps = 0.0;
    uint64_t peak_rss_bytes = 0;
    std::vector<uint64_t> rss_samples_bytes;
};

struct CafBenchReport {
    std::string schema_version = "1";
    CafBenchConfig config;
    std::vector<TrialMetrics> trials;
};
```

Scenario entry points produced by Tasks 3-5:

```cpp
TrialMetrics run_actor_creation_trial(const CafBenchConfig& cfg,
                                      uint32_t trial_index);
TrialMetrics run_mailbox_n1_trial(const CafBenchConfig& cfg,
                                  uint32_t trial_index);
TrialMetrics run_mixed_case_trial(const CafBenchConfig& cfg,
                                  uint32_t trial_index);
```

Runner entry points produced in Task 6:

```cpp
CafBenchReport run_caf_benchmark(const CafBenchConfig& cfg);
int run_caf_benchmark_main(int argc, const char* const* argv);
```

---

### Task 1: App Scaffold, Config Parser, And Output Primitives

**Files:**
- Create: `apps/bench_caf/CMakeLists.txt`
- Create: `apps/bench_caf/18_bench_caf.cpp`
- Create: `apps/bench_caf/caf_bench_config.hpp`
- Create: `apps/bench_caf/caf_bench_metrics.hpp`
- Create: `apps/bench_caf/caf_bench_output.hpp`
- Create: `tests/unit/apps/test_bench_caf_config.cpp`
- Modify: `apps/CMakeLists.txt`
- Modify: `tests/unit/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: existing HPActor app CMake pattern from `apps/bench_saturate/CMakeLists.txt`.
- Produces: `CafBenchConfig`, `TrialMetrics`, `CafBenchReport`, `parse_caf_bench_args()`, `validate_config()`, `write_json_report()`, and `write_csv_report()`.

- [ ] **Step 1: Write the failing config and output tests**

Create `tests/unit/apps/test_bench_caf_config.cpp`:

```cpp
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

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_metrics.hpp>
#include <apps/bench_caf/caf_bench_output.hpp>

#include <gtest/gtest.h>

#include <string>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafConfig, ParsesMailboxSmokeJson) {
    const char* argv[] = {
        "18_bench_caf",
        "--scenario",
        "mailbox-n1",
        "--preset",
        "smoke",
        "--scheduler-threads",
        "4",
        "--message-size",
        "1024",
        "--message-shape",
        "fixed-bytes",
        "--trials",
        "3",
        "--format",
        "json",
    };

    auto parsed = bench_caf::parse_caf_bench_args(15, argv);
    ASSERT_TRUE(parsed.ok) << parsed.error;
    EXPECT_EQ(parsed.config.scenario, bench_caf::ScenarioKind::MailboxN1);
    EXPECT_EQ(parsed.config.preset, bench_caf::PresetKind::Smoke);
    EXPECT_EQ(parsed.config.scheduler_threads, 4u);
    EXPECT_EQ(parsed.config.message_size_bytes, 1024u);
    EXPECT_EQ(parsed.config.message_shape, bench_caf::MessageShape::FixedBytes);
    EXPECT_EQ(parsed.config.trials, 3u);
    EXPECT_EQ(parsed.config.format, bench_caf::OutputFormat::Json);
}

TEST(BenchCafConfig, RejectsZeroTrials) {
    const char* argv[] = {
        "18_bench_caf",
        "--scenario",
        "actor-creation",
        "--trials",
        "0",
    };

    auto parsed = bench_caf::parse_caf_bench_args(5, argv);
    ASSERT_FALSE(parsed.ok);
    EXPECT_NE(parsed.error.find("trials must be greater than zero"),
              std::string::npos);
}

TEST(BenchCafOutput, JsonContainsScenarioAndTrialCounters) {
    bench_caf::CafBenchReport report;
    report.config.scenario = bench_caf::ScenarioKind::ActorCreation;
    report.config.preset = bench_caf::PresetKind::Smoke;
    report.config.scheduler_threads = 2;

    bench_caf::TrialMetrics trial;
    trial.trial = 1;
    trial.completed = true;
    trial.runtime_ms = 7;
    trial.actors_created = 2047;
    trial.actors_completed = 2047;
    report.trials.push_back(trial);

    auto json = bench_caf::write_json_report(report);
    EXPECT_NE(json.find("\"schema_version\": \"1\""), std::string::npos);
    EXPECT_NE(json.find("\"scenario\": \"actor-creation\""), std::string::npos);
    EXPECT_NE(json.find("\"actors_created\": 2047"), std::string::npos);
}

TEST(BenchCafOutput, CsvHasStableHeaderAndOneTrialRow) {
    bench_caf::CafBenchReport report;
    report.config.scenario = bench_caf::ScenarioKind::MailboxN1;
    report.config.preset = bench_caf::PresetKind::Smoke;
    report.config.scheduler_threads = 2;

    bench_caf::TrialMetrics trial;
    trial.trial = 1;
    trial.completed = true;
    trial.total_sent = 10;
    trial.total_received = 10;
    report.trials.push_back(trial);

    auto csv = bench_caf::write_csv_report(report);
    EXPECT_NE(csv.find("scenario,preset,trial,completed"), std::string::npos);
    EXPECT_NE(csv.find("mailbox-n1,smoke,1,true"), std::string::npos);
}
```

Modify `tests/unit/apps/CMakeLists.txt`:

```cmake
add_executable(test_unit_apps
    test_cli_demo_actor_factory.cpp
    test_bench_caf_config.cpp
    ${CMAKE_SOURCE_DIR}/apps/hpactor_demo/cli_demo_actor_factory.cpp
)
target_link_libraries(test_unit_apps hpactor hpactor_test_support GTest::gtest_main)
target_include_directories(test_unit_apps PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/apps/cli_demo
)
gtest_discover_tests(test_unit_apps)
```

- [ ] **Step 2: Run the test to verify RED**

Run:

```bash
cmake -S . -B build -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DENABLE_APPS=ON
ninja -C build test_unit_apps
```

Expected: compile fails because `apps/bench_caf/caf_bench_config.hpp` does not exist.

- [ ] **Step 3: Add the app scaffold**

Create `apps/bench_caf/CMakeLists.txt`:

```cmake
add_executable(18_bench_caf
    18_bench_caf.cpp
)
target_link_libraries(18_bench_caf PRIVATE hpactor_lib)
target_include_directories(18_bench_caf PRIVATE ${CMAKE_SOURCE_DIR})
```

Modify `apps/CMakeLists.txt`:

```cmake
add_subdirectory(order_platform)
add_subdirectory(edgeops_telemetry)
add_subdirectory(cli_demo)
add_subdirectory(bench_perf)
add_subdirectory(hpactor_demo)
add_subdirectory(bench_saturate)
add_subdirectory(bench_caf)
add_subdirectory(cluster_control_plane)
```

Create `apps/bench_caf/18_bench_caf.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include "caf_bench_config.hpp"
#include "caf_bench_output.hpp"

#include <fstream>
#include <iostream>

namespace hpactor::apps::bench_caf {

int run_caf_benchmark_main(int argc, const char* const* argv) {
    auto parsed = parse_caf_bench_args(argc, argv);
    if (!parsed.ok) {
        std::cerr << parsed.error << '\n';
        return 1;
    }

    CafBenchReport report;
    report.config = parsed.config;

    std::string output = parsed.config.format == OutputFormat::Csv
                             ? write_csv_report(report)
                             : write_json_report(report);

    if (!parsed.config.output_path.empty()) {
        std::ofstream out(parsed.config.output_path);
        if (!out) {
            std::cerr << "failed to open output path: "
                      << parsed.config.output_path << '\n';
            return 5;
        }
        out << output;
    } else {
        std::cout << output;
    }
    return 0;
}

} // namespace hpactor::apps::bench_caf

int main(int argc, const char* const* argv) {
    return hpactor::apps::bench_caf::run_caf_benchmark_main(argc, argv);
}
```

- [ ] **Step 4: Add config, metrics, and output primitives**

Create `apps/bench_caf/caf_bench_config.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <cstdint>
#include <cstdlib>
#include <string>

namespace hpactor::apps::bench_caf {

enum class ScenarioKind {
    ActorCreation,
    MailboxN1,
    MixedCase,
};

enum class PresetKind {
    Smoke,
    Nightly,
    PaperScale,
    Stress,
};

enum class OutputFormat {
    Json,
    Csv,
};

enum class MessageShape {
    HeaderOnly,
    FixedBytes,
};

enum class TrafficDistribution {
    NToOne,
};

struct CafBenchConfig {
    ScenarioKind scenario = ScenarioKind::ActorCreation;
    PresetKind preset = PresetKind::Smoke;
    OutputFormat format = OutputFormat::Json;
    MessageShape message_shape = MessageShape::HeaderOnly;
    TrafficDistribution distribution = TrafficDistribution::NToOne;
    uint32_t scheduler_threads = 2;
    uint32_t trials = 1;
    uint32_t warmups = 0;
    uint32_t message_size_bytes = 0;
    uint32_t mailbox_capacity = 4096;
    uint64_t seed = 1;
    uint32_t sample_rss_ms = 50;
    std::string output_path;
};

struct ParseResult {
    bool ok = false;
    CafBenchConfig config;
    std::string error;
};

inline const char* scenario_name(ScenarioKind scenario) {
    switch (scenario) {
    case ScenarioKind::ActorCreation:
        return "actor-creation";
    case ScenarioKind::MailboxN1:
        return "mailbox-n1";
    case ScenarioKind::MixedCase:
        return "mixed-case";
    }
    return "actor-creation";
}

inline const char* preset_name(PresetKind preset) {
    switch (preset) {
    case PresetKind::Smoke:
        return "smoke";
    case PresetKind::Nightly:
        return "nightly";
    case PresetKind::PaperScale:
        return "paper-scale";
    case PresetKind::Stress:
        return "stress";
    }
    return "smoke";
}

inline const char* output_format_name(OutputFormat format) {
    return format == OutputFormat::Csv ? "csv" : "json";
}

inline const char* message_shape_name(MessageShape shape) {
    return shape == MessageShape::FixedBytes ? "fixed-bytes" : "header-only";
}

inline bool parse_scenario(const std::string& value, ScenarioKind& out) {
    if (value == "actor-creation") {
        out = ScenarioKind::ActorCreation;
        return true;
    }
    if (value == "mailbox-n1") {
        out = ScenarioKind::MailboxN1;
        return true;
    }
    if (value == "mixed-case") {
        out = ScenarioKind::MixedCase;
        return true;
    }
    return false;
}

inline bool parse_preset(const std::string& value, PresetKind& out) {
    if (value == "smoke") {
        out = PresetKind::Smoke;
        return true;
    }
    if (value == "nightly") {
        out = PresetKind::Nightly;
        return true;
    }
    if (value == "paper-scale") {
        out = PresetKind::PaperScale;
        return true;
    }
    if (value == "stress") {
        out = PresetKind::Stress;
        return true;
    }
    return false;
}

inline bool parse_format(const std::string& value, OutputFormat& out) {
    if (value == "json") {
        out = OutputFormat::Json;
        return true;
    }
    if (value == "csv") {
        out = OutputFormat::Csv;
        return true;
    }
    return false;
}

inline bool parse_message_shape(const std::string& value, MessageShape& out) {
    if (value == "header-only") {
        out = MessageShape::HeaderOnly;
        return true;
    }
    if (value == "fixed-bytes") {
        out = MessageShape::FixedBytes;
        return true;
    }
    return false;
}

inline bool parse_u32(const char* text, uint32_t& out) {
    char* end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0')
        return false;
    out = static_cast<uint32_t>(value);
    return true;
}

inline bool parse_u64(const char* text, uint64_t& out) {
    char* end = nullptr;
    unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0')
        return false;
    out = static_cast<uint64_t>(value);
    return true;
}

inline std::string validate_config(const CafBenchConfig& cfg) {
    if (cfg.trials == 0)
        return "trials must be greater than zero";
    if (cfg.scheduler_threads == 0)
        return "scheduler-threads must be greater than zero";
    if (cfg.sample_rss_ms == 0)
        return "sample-rss-ms must be greater than zero";
    if (cfg.preset == PresetKind::Smoke && cfg.message_size_bytes > 65536)
        return "smoke preset message-size must be at most 65536 bytes";
    return {};
}

inline ParseResult parse_caf_bench_args(int argc, const char* const* argv) {
    CafBenchConfig cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const char* flag) -> const char* {
            if (i + 1 >= argc)
                return nullptr;
            ++i;
            return argv[i];
        };

        if (arg == "--scenario") {
            const char* value = require_value("--scenario");
            if (!value || !parse_scenario(value, cfg.scenario))
                return {false, cfg, "invalid --scenario"};
        } else if (arg == "--preset") {
            const char* value = require_value("--preset");
            if (!value || !parse_preset(value, cfg.preset))
                return {false, cfg, "invalid --preset"};
        } else if (arg == "--format") {
            const char* value = require_value("--format");
            if (!value || !parse_format(value, cfg.format))
                return {false, cfg, "invalid --format"};
        } else if (arg == "--message-shape") {
            const char* value = require_value("--message-shape");
            if (!value || !parse_message_shape(value, cfg.message_shape))
                return {false, cfg, "invalid --message-shape"};
        } else if (arg == "--scheduler-threads") {
            const char* value = require_value("--scheduler-threads");
            if (!value || !parse_u32(value, cfg.scheduler_threads))
                return {false, cfg, "invalid --scheduler-threads"};
        } else if (arg == "--trials") {
            const char* value = require_value("--trials");
            if (!value || !parse_u32(value, cfg.trials))
                return {false, cfg, "invalid --trials"};
        } else if (arg == "--warmup") {
            const char* value = require_value("--warmup");
            if (!value || !parse_u32(value, cfg.warmups))
                return {false, cfg, "invalid --warmup"};
        } else if (arg == "--message-size") {
            const char* value = require_value("--message-size");
            if (!value || !parse_u32(value, cfg.message_size_bytes))
                return {false, cfg, "invalid --message-size"};
        } else if (arg == "--mailbox-capacity") {
            const char* value = require_value("--mailbox-capacity");
            if (!value || !parse_u32(value, cfg.mailbox_capacity))
                return {false, cfg, "invalid --mailbox-capacity"};
        } else if (arg == "--seed") {
            const char* value = require_value("--seed");
            if (!value || !parse_u64(value, cfg.seed))
                return {false, cfg, "invalid --seed"};
        } else if (arg == "--sample-rss-ms") {
            const char* value = require_value("--sample-rss-ms");
            if (!value || !parse_u32(value, cfg.sample_rss_ms))
                return {false, cfg, "invalid --sample-rss-ms"};
        } else if (arg == "--output") {
            const char* value = require_value("--output");
            if (!value)
                return {false, cfg, "invalid --output"};
            cfg.output_path = value;
        } else {
            return {false, cfg, "unknown argument: " + arg};
        }
    }

    auto validation = validate_config(cfg);
    if (!validation.empty())
        return {false, cfg, validation};
    return {true, cfg, {}};
}

} // namespace hpactor::apps::bench_caf
```

Create `apps/bench_caf/caf_bench_metrics.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include "caf_bench_config.hpp"

#include <cstdint>
#include <vector>

namespace hpactor::apps::bench_caf {

struct TrialMetrics {
    uint32_t trial = 0;
    bool completed = false;
    uint64_t runtime_ms = 0;
    uint64_t total_sent = 0;
    uint64_t total_received = 0;
    uint64_t total_rejected = 0;
    uint64_t total_dropped = 0;
    uint64_t actors_created = 0;
    uint64_t actors_completed = 0;
    uint64_t rings_completed = 0;
    uint64_t token_hops = 0;
    uint64_t cpu_tasks_completed = 0;
    double throughput_msgps = 0.0;
    uint64_t peak_rss_bytes = 0;
    std::vector<uint64_t> rss_samples_bytes;
};

struct CafBenchReport {
    std::string schema_version = "1";
    CafBenchConfig config;
    std::vector<TrialMetrics> trials;
};

inline uint64_t peak_rss(const std::vector<uint64_t>& samples) {
    uint64_t peak = 0;
    for (auto value : samples) {
        if (value > peak)
            peak = value;
    }
    return peak;
}

inline double throughput(uint64_t messages, uint64_t runtime_ms) {
    if (runtime_ms == 0)
        return 0.0;
    return static_cast<double>(messages) * 1000.0 /
           static_cast<double>(runtime_ms);
}

} // namespace hpactor::apps::bench_caf
```

Create `apps/bench_caf/caf_bench_output.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include "caf_bench_metrics.hpp"

#include <iomanip>
#include <sstream>
#include <string>

namespace hpactor::apps::bench_caf {

inline std::string bool_text(bool value) {
    return value ? "true" : "false";
}

inline std::string write_json_report(const CafBenchReport& report) {
    std::ostringstream out;
    out << "{\n";
    out << "  \"schema_version\": \"" << report.schema_version << "\",\n";
    out << "  \"benchmark\": \"caf-port\",\n";
    out << "  \"scenario\": \"" << scenario_name(report.config.scenario)
        << "\",\n";
    out << "  \"preset\": \"" << preset_name(report.config.preset) << "\",\n";
    out << "  \"runtime\": {\n";
    out << "    \"scheduler_threads\": " << report.config.scheduler_threads
        << ",\n";
    out << "    \"mailbox_capacity\": " << report.config.mailbox_capacity
        << "\n";
    out << "  },\n";
    out << "  \"parameters\": {\n";
    out << "    \"message_size_bytes\": " << report.config.message_size_bytes
        << ",\n";
    out << "    \"message_shape\": \""
        << message_shape_name(report.config.message_shape) << "\",\n";
    out << "    \"seed\": " << report.config.seed << "\n";
    out << "  },\n";
    out << "  \"trials\": [\n";
    for (size_t i = 0; i < report.trials.size(); ++i) {
        const auto& t = report.trials[i];
        out << "    {\n";
        out << "      \"trial\": " << t.trial << ",\n";
        out << "      \"completed\": " << bool_text(t.completed) << ",\n";
        out << "      \"runtime_ms\": " << t.runtime_ms << ",\n";
        out << "      \"total_sent\": " << t.total_sent << ",\n";
        out << "      \"total_received\": " << t.total_received << ",\n";
        out << "      \"total_rejected\": " << t.total_rejected << ",\n";
        out << "      \"total_dropped\": " << t.total_dropped << ",\n";
        out << "      \"actors_created\": " << t.actors_created << ",\n";
        out << "      \"actors_completed\": " << t.actors_completed << ",\n";
        out << "      \"rings_completed\": " << t.rings_completed << ",\n";
        out << "      \"token_hops\": " << t.token_hops << ",\n";
        out << "      \"cpu_tasks_completed\": " << t.cpu_tasks_completed
            << ",\n";
        out << "      \"throughput_msgps\": " << std::fixed
            << std::setprecision(3) << t.throughput_msgps << ",\n";
        out << "      \"peak_rss_bytes\": " << t.peak_rss_bytes << "\n";
        out << "    }";
        if (i + 1 < report.trials.size())
            out << ",";
        out << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    return out.str();
}

inline std::string write_csv_report(const CafBenchReport& report) {
    std::ostringstream out;
    out << "scenario,preset,trial,completed,runtime_ms,total_sent,"
        << "total_received,total_rejected,total_dropped,actors_created,"
        << "actors_completed,rings_completed,token_hops,cpu_tasks_completed,"
        << "throughput_msgps,peak_rss_bytes\n";
    for (const auto& t : report.trials) {
        out << scenario_name(report.config.scenario) << ','
            << preset_name(report.config.preset) << ',' << t.trial << ','
            << bool_text(t.completed) << ',' << t.runtime_ms << ','
            << t.total_sent << ',' << t.total_received << ','
            << t.total_rejected << ',' << t.total_dropped << ','
            << t.actors_created << ',' << t.actors_completed << ','
            << t.rings_completed << ',' << t.token_hops << ','
            << t.cpu_tasks_completed << ',' << std::fixed
            << std::setprecision(3) << t.throughput_msgps << ','
            << t.peak_rss_bytes << '\n';
    }
    return out.str();
}

} // namespace hpactor::apps::bench_caf
```

- [ ] **Step 5: Run the config/output tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="BenchCaf*"
```

Expected: `BenchCafConfig.*` and `BenchCafOutput.*` tests pass.

- [ ] **Step 6: Commit Task 1**

Run:

```bash
git add apps/CMakeLists.txt apps/bench_caf tests/unit/apps/CMakeLists.txt tests/unit/apps/test_bench_caf_config.cpp
git commit -m "feat(bench-caf): add config and output scaffold"
```

---

### Task 2: Metrics Helpers, RSS Sampler, And Benchmark Payloads

**Files:**
- Create: `apps/bench_caf/caf_bench_sampler.hpp`
- Create: `apps/bench_caf/messages.hpp`
- Create: `tests/unit/apps/test_bench_caf_metrics.cpp`
- Create: `tests/unit/apps/test_bench_caf_payloads.cpp`
- Modify: `tests/unit/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `CafBenchConfig`, `TrialMetrics`, `peak_rss()`, `throughput()`.
- Produces: `RssSampler`, `BenchPayloadHeader`, `encode_bench_payload()`, `decode_bench_payload()`, `make_bench_msg()`.

- [ ] **Step 1: Write failing metrics and payload tests**

Create `tests/unit/apps/test_bench_caf_metrics.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/caf_bench_metrics.hpp>
#include <apps/bench_caf/caf_bench_sampler.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafMetrics, ComputesThroughput) {
    EXPECT_DOUBLE_EQ(bench_caf::throughput(2000, 1000), 2000.0);
    EXPECT_DOUBLE_EQ(bench_caf::throughput(5, 0), 0.0);
}

TEST(BenchCafMetrics, ComputesPeakRss) {
    EXPECT_EQ(bench_caf::peak_rss({7, 11, 3}), 11u);
    EXPECT_EQ(bench_caf::peak_rss({}), 0u);
}

TEST(BenchCafSampler, SnapshotIsNonNegative) {
    auto sample = bench_caf::sample_current_rss_bytes();
    EXPECT_GE(sample, 0u);
}
```

Create `tests/unit/apps/test_bench_caf_payloads.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/messages.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafPayloads, HeaderOnlyRoundTrips) {
    bench_caf::BenchPayloadHeader header;
    header.sender_id = 3;
    header.sequence = 9;
    header.timestamp_us = 17;

    auto payload = bench_caf::encode_bench_payload(header, 0, 123);
    auto decoded = bench_caf::decode_bench_payload(payload);

    EXPECT_EQ(payload.size(), bench_caf::BenchPayloadHeader::kEncodedSize);
    EXPECT_EQ(decoded.sender_id, 3u);
    EXPECT_EQ(decoded.sequence, 9u);
    EXPECT_EQ(decoded.timestamp_us, 17u);
}

TEST(BenchCafPayloads, FixedBytesRespectsMinimumHeaderSize) {
    bench_caf::BenchPayloadHeader header;
    auto payload = bench_caf::encode_bench_payload(header, 4, 123);
    EXPECT_EQ(payload.size(), bench_caf::BenchPayloadHeader::kEncodedSize);
}

TEST(BenchCafPayloads, FixedBytesUsesRequestedSize) {
    bench_caf::BenchPayloadHeader header;
    auto payload = bench_caf::encode_bench_payload(header, 1024, 123);
    EXPECT_EQ(payload.size(), 1024u);
}
```

Modify `tests/unit/apps/CMakeLists.txt`:

```cmake
add_executable(test_unit_apps
    test_cli_demo_actor_factory.cpp
    test_bench_caf_config.cpp
    test_bench_caf_metrics.cpp
    test_bench_caf_payloads.cpp
    ${CMAKE_SOURCE_DIR}/apps/hpactor_demo/cli_demo_actor_factory.cpp
)
```

- [ ] **Step 2: Run the tests to verify RED**

Run:

```bash
ninja -C build test_unit_apps
```

Expected: compile fails because `caf_bench_sampler.hpp` and `messages.hpp` do not exist.

- [ ] **Step 3: Add the RSS sampler**

Create `apps/bench_caf/caf_bench_sampler.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

namespace hpactor::apps::bench_caf {

inline uint64_t sample_current_rss_bytes() {
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string key;
    uint64_t value = 0;
    std::string unit;
    while (status >> key >> value >> unit) {
        if (key == "VmRSS:")
            return value * 1024;
    }
    return 0;
#elif defined(__APPLE__)
    mach_task_basic_info info;
    mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
    auto result = task_info(mach_task_self(), MACH_TASK_BASIC_INFO,
                            reinterpret_cast<task_info_t>(&info), &count);
    if (result != KERN_SUCCESS)
        return 0;
    return static_cast<uint64_t>(info.resident_size);
#else
    return 0;
#endif
}

class RssSampler {
  public:
    explicit RssSampler(uint32_t interval_ms)
        : interval_ms_(interval_ms == 0 ? 50 : interval_ms) {}

    ~RssSampler() {
        stop();
    }

    void start() {
        running_.store(true, std::memory_order_release);
        samples_.clear();
        worker_ = std::thread([this]() {
            while (running_.load(std::memory_order_acquire)) {
                samples_.push_back(sample_current_rss_bytes());
                std::this_thread::sleep_for(
                    std::chrono::milliseconds(interval_ms_));
            }
            samples_.push_back(sample_current_rss_bytes());
        });
    }

    std::vector<uint64_t> stop() {
        running_.store(false, std::memory_order_release);
        if (worker_.joinable())
            worker_.join();
        return samples_;
    }

  private:
    uint32_t interval_ms_ = 50;
    std::atomic<bool> running_{false};
    std::thread worker_;
    std::vector<uint64_t> samples_;
};

} // namespace hpactor::apps::bench_caf
```

- [ ] **Step 4: Add benchmark payload helpers**

Create `apps/bench_caf/messages.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace hpactor::apps::bench_caf {

inline constexpr TypeTag ActorCreationStartTag{0x00010300};
inline constexpr TypeTag ActorCreationDoneTag{0x00010301};
inline constexpr TypeTag MailboxLoadTag{0x00010302};
inline constexpr TypeTag MailboxDoneTag{0x00010303};
inline constexpr TypeTag MixedTokenTag{0x00010304};
inline constexpr TypeTag MixedDoneTag{0x00010305};
inline constexpr TypeTag MixedCpuTaskTag{0x00010306};
inline constexpr TypeTag MixedCpuDoneTag{0x00010307};

struct BenchPayloadHeader {
    static constexpr size_t kEncodedSize = 20;

    uint32_t sender_id = 0;
    uint64_t sequence = 0;
    uint64_t timestamp_us = 0;
};

inline StreamBuffer encode_bench_payload(const BenchPayloadHeader& header,
                                         size_t requested_size,
                                         uint64_t seed) {
    size_t size = std::max(requested_size, BenchPayloadHeader::kEncodedSize);
    StreamBuffer payload(size);
    uint8_t* data = payload.data();
    size_t off = 0;
    std::memcpy(data + off, &header.sender_id, sizeof(header.sender_id));
    off += sizeof(header.sender_id);
    std::memcpy(data + off, &header.sequence, sizeof(header.sequence));
    off += sizeof(header.sequence);
    std::memcpy(data + off, &header.timestamp_us, sizeof(header.timestamp_us));
    off += sizeof(header.timestamp_us);

    uint64_t value = seed;
    for (size_t i = off; i < size; ++i) {
        value = value * 6364136223846793005ULL + 1442695040888963407ULL;
        data[i] = static_cast<uint8_t>(value >> 32);
    }
    return payload;
}

inline BenchPayloadHeader decode_bench_payload(const StreamBuffer& payload) {
    BenchPayloadHeader header;
    if (payload.size() < BenchPayloadHeader::kEncodedSize)
        return header;
    const uint8_t* data = payload.data();
    size_t off = 0;
    std::memcpy(&header.sender_id, data + off, sizeof(header.sender_id));
    off += sizeof(header.sender_id);
    std::memcpy(&header.sequence, data + off, sizeof(header.sequence));
    off += sizeof(header.sequence);
    std::memcpy(&header.timestamp_us, data + off, sizeof(header.timestamp_us));
    return header;
}

inline TypedMessage make_bench_msg(TypeTag tag, StreamBuffer payload = {}) {
    return TypedMessage(tag, std::move(payload));
}

} // namespace hpactor::apps::bench_caf
```

- [ ] **Step 5: Run the metrics and payload tests to verify GREEN**

Run:

```bash
ninja -C build test_unit_apps
./build/tests/unit/apps/test_unit_apps --gtest_filter="BenchCafMetrics.*:BenchCafSampler.*:BenchCafPayloads.*"
```

Expected: all selected tests pass.

- [ ] **Step 6: Commit Task 2**

Run:

```bash
git add apps/bench_caf/caf_bench_sampler.hpp apps/bench_caf/messages.hpp tests/unit/apps/CMakeLists.txt tests/unit/apps/test_bench_caf_metrics.cpp tests/unit/apps/test_bench_caf_payloads.cpp
git commit -m "feat(bench-caf): add metrics sampler and payload helpers"
```

---

### Task 3: Actor Creation Scenario

**Files:**
- Create: `apps/bench_caf/actors/actor_creation_actor.hpp`
- Create: `apps/bench_caf/caf_bench_scenarios.hpp`
- Create: `tests/integration/apps/test_bench_caf_actor_creation.cpp`
- Modify: `tests/integration/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `CafBenchConfig`, `TrialMetrics`, `RssSampler`, `ActorSystem`.
- Produces: `run_actor_creation_trial(const CafBenchConfig&, uint32_t)`.

- [ ] **Step 1: Write the failing actor-creation integration test**

Create `tests/integration/apps/test_bench_caf_actor_creation.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_scenarios.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafActorCreation, SmokeCompletesExpectedTree) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::ActorCreation;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.sample_rss_ms = 10;

    auto metrics = bench_caf::run_actor_creation_trial(cfg, 1);

    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.trial, 1u);
    EXPECT_EQ(metrics.actors_created, 2047u);
    EXPECT_EQ(metrics.actors_completed, 2047u);
    EXPECT_GT(metrics.runtime_ms, 0u);
}
```

Modify `tests/integration/apps/CMakeLists.txt` by adding a new target:

```cmake
add_executable(test_bench_caf_actor_creation test_bench_caf_actor_creation.cpp)
target_include_directories(test_bench_caf_actor_creation PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_caf_actor_creation hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_caf_actor_creation PROPERTIES TIMEOUT 30)
```

- [ ] **Step 2: Run the integration test to verify RED**

Run:

```bash
ninja -C build test_bench_caf_actor_creation
```

Expected: compile fails because `caf_bench_scenarios.hpp` does not exist.

- [ ] **Step 3: Add actor-creation scenario implementation**

Create `apps/bench_caf/actors/actor_creation_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include "../messages.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

struct ActorCreationCounters {
    std::atomic<uint64_t> created{0};
    std::atomic<uint64_t> completed{0};
};

class ActorCreationNodeActor : public EventBasedActor {
  public:
    ActorCreationNodeActor(ActorContext* ctx, ActorSystem& sys,
                           ActorCreationCounters* counters, uint32_t depth)
        : EventBasedActor(ctx, sys), counters_(counters), depth_(depth) {
        counters_->created.fetch_add(1, std::memory_order_relaxed);
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != ActorCreationStartTag)
                return;
            run_node();
        }};
    }

  private:
    void run_node() {
        if (depth_ == 0) {
            counters_->completed.fetch_add(1, std::memory_order_release);
            stop();
            return;
        }

        auto left = system().spawn<ActorCreationNodeActor>(counters_, depth_ - 1);
        auto right = system().spawn<ActorCreationNodeActor>(counters_, depth_ - 1);
        system().deliver_local(left.id(), make_bench_msg(ActorCreationStartTag));
        system().deliver_local(right.id(), make_bench_msg(ActorCreationStartTag));
        counters_->completed.fetch_add(1, std::memory_order_release);
        stop();
    }

    ActorCreationCounters* counters_ = nullptr;
    uint32_t depth_ = 0;
};

inline uint32_t actor_creation_depth_for_preset(PresetKind preset) {
    switch (preset) {
    case PresetKind::Smoke:
        return 10;
    case PresetKind::Nightly:
        return 16;
    case PresetKind::PaperScale:
        return 20;
    case PresetKind::Stress:
        return 21;
    }
    return 10;
}

inline uint64_t actor_creation_expected_count(uint32_t depth) {
    return (uint64_t{1} << (depth + 1)) - 1;
}

} // namespace hpactor::apps::bench_caf
```

Create `apps/bench_caf/caf_bench_scenarios.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include "actors/actor_creation_actor.hpp"
#include "caf_bench_config.hpp"
#include "caf_bench_metrics.hpp"
#include "caf_bench_sampler.hpp"

#include <hpactor/actor/actor_system.hpp>

#include <chrono>
#include <thread>

namespace hpactor::apps::bench_caf {

inline Config make_bench_actor_config(const CafBenchConfig& cfg) {
    Config runtime;
    runtime.scheduler_threads = cfg.scheduler_threads;
    runtime.max_queue_depth = cfg.mailbox_capacity;
    runtime.mailbox.default_capacity = cfg.mailbox_capacity;
    runtime.enable_network = false;
    runtime.enable_receptionist = false;
    runtime.cli.enabled = false;
    runtime.tracing.enabled = false;
    return runtime;
}

inline TrialMetrics run_actor_creation_trial(const CafBenchConfig& cfg,
                                             uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    ActorCreationCounters counters;
    uint32_t depth = actor_creation_depth_for_preset(cfg.preset);
    uint64_t expected = actor_creation_expected_count(depth);

    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));
    auto root = system.spawn<ActorCreationNodeActor>(&counters, depth);
    system.deliver_local(root.id(), make_bench_msg(ActorCreationStartTag));

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.completed.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count());
    metrics.actors_created = counters.created.load();
    metrics.actors_completed = counters.completed.load();
    metrics.completed = shutdown.has_value() &&
                        metrics.actors_created == expected &&
                        metrics.actors_completed == expected;
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}

} // namespace hpactor::apps::bench_caf
```

- [ ] **Step 4: Run actor-creation test to verify GREEN**

Run:

```bash
ninja -C build test_bench_caf_actor_creation
./build/tests/integration/apps/test_bench_caf_actor_creation
```

Expected: `BenchCafActorCreation.SmokeCompletesExpectedTree` passes.

- [ ] **Step 5: Commit Task 3**

Run:

```bash
git add apps/bench_caf/actors/actor_creation_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp tests/integration/apps/CMakeLists.txt tests/integration/apps/test_bench_caf_actor_creation.cpp
git commit -m "feat(bench-caf): add actor creation scenario"
```

---

### Task 4: Mailbox N:1 Scenario

**Files:**
- Create: `apps/bench_caf/actors/mailbox_n1_actor.hpp`
- Create: `tests/integration/apps/test_bench_caf_mailbox_n1.cpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `tests/integration/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `CafBenchConfig`, `TrialMetrics`, `make_bench_actor_config()`, payload helpers.
- Produces: `run_mailbox_n1_trial(const CafBenchConfig&, uint32_t)`.

- [ ] **Step 1: Write the failing mailbox integration test**

Create `tests/integration/apps/test_bench_caf_mailbox_n1.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_scenarios.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafMailboxN1, SmokeCompletesAllMessages) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.mailbox_capacity = 4096;
    cfg.sample_rss_ms = 10;

    auto metrics = bench_caf::run_mailbox_n1_trial(cfg, 1);

    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.trial, 1u);
    EXPECT_EQ(metrics.total_sent, 40000u);
    EXPECT_EQ(metrics.total_received, 40000u);
    EXPECT_EQ(metrics.total_dropped, 0u);
    EXPECT_GT(metrics.throughput_msgps, 0.0);
}
```

Modify `tests/integration/apps/CMakeLists.txt`:

```cmake
add_executable(test_bench_caf_mailbox_n1 test_bench_caf_mailbox_n1.cpp)
target_include_directories(test_bench_caf_mailbox_n1 PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_caf_mailbox_n1 hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_caf_mailbox_n1 PROPERTIES TIMEOUT 30)
```

- [ ] **Step 2: Run mailbox test to verify RED**

Run:

```bash
ninja -C build test_bench_caf_mailbox_n1
```

Expected: compile fails because `run_mailbox_n1_trial()` is not defined.

- [ ] **Step 3: Add mailbox actors**

Create `apps/bench_caf/actors/mailbox_n1_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include "../caf_bench_config.hpp"
#include "../messages.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

struct MailboxN1Counters {
    std::atomic<uint64_t> sent{0};
    std::atomic<uint64_t> received{0};
    std::atomic<uint64_t> senders_done{0};
};

struct MailboxN1Dimensions {
    uint32_t senders = 4;
    uint32_t messages_per_sender = 10000;
};

inline MailboxN1Dimensions mailbox_n1_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
    case PresetKind::Smoke:
        return {4, 10000};
    case PresetKind::Nightly:
        return {32, 100000};
    case PresetKind::PaperScale:
        return {100, 1000000};
    case PresetKind::Stress:
        return {128, 1000000};
    }
    return {4, 10000};
}

class MailboxN1ReceiverActor : public EventBasedActor {
  public:
    MailboxN1ReceiverActor(ActorContext* ctx, ActorSystem& sys,
                           MailboxN1Counters* counters)
        : EventBasedActor(ctx, sys), counters_(counters) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() == MailboxLoadTag) {
                counters_->received.fetch_add(1, std::memory_order_relaxed);
            }
        }};
    }

  private:
    MailboxN1Counters* counters_ = nullptr;
};

class MailboxN1SenderActor : public EventBasedActor {
  public:
    MailboxN1SenderActor(ActorContext* ctx, ActorSystem& sys,
                         MailboxN1Counters* counters, ActorAddress receiver,
                         uint32_t sender_id, uint32_t messages_to_send,
                         uint32_t payload_size, uint64_t seed)
        : EventBasedActor(ctx, sys), counters_(counters), receiver_(receiver),
          sender_id_(sender_id), messages_to_send_(messages_to_send),
          payload_size_(payload_size), seed_(seed) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MailboxLoadTag)
                return;
            for (uint32_t i = 0; i < messages_to_send_; ++i) {
                BenchPayloadHeader header;
                header.sender_id = sender_id_;
                header.sequence = i;
                auto payload =
                    encode_bench_payload(header, payload_size_, seed_ + i);
                context()->send(receiver_, make_bench_msg(MailboxLoadTag,
                                                          std::move(payload)));
                counters_->sent.fetch_add(1, std::memory_order_relaxed);
            }
            counters_->senders_done.fetch_add(1, std::memory_order_release);
            stop();
        }};
    }

  private:
    MailboxN1Counters* counters_ = nullptr;
    ActorAddress receiver_;
    uint32_t sender_id_ = 0;
    uint32_t messages_to_send_ = 0;
    uint32_t payload_size_ = 0;
    uint64_t seed_ = 0;
};

} // namespace hpactor::apps::bench_caf
```

- [ ] **Step 4: Add mailbox scenario runner**

Modify `apps/bench_caf/caf_bench_scenarios.hpp`:

```cpp
#include "actors/mailbox_n1_actor.hpp"
```

Add below `run_actor_creation_trial()`:

```cpp
inline TrialMetrics run_mailbox_n1_trial(const CafBenchConfig& cfg,
                                         uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = mailbox_n1_dimensions_for_preset(cfg.preset);
    uint64_t expected =
        static_cast<uint64_t>(dims.senders) * dims.messages_per_sender;

    MailboxN1Counters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));
    auto receiver = system.spawn<MailboxN1ReceiverActor>(&counters);

    for (uint32_t i = 0; i < dims.senders; ++i) {
        auto sender = system.spawn<MailboxN1SenderActor>(
            &counters, receiver.address(), i, dims.messages_per_sender,
            cfg.message_size_bytes, cfg.seed + i);
        system.deliver_local(sender.id(), make_bench_msg(MailboxLoadTag));
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (counters.received.load(std::memory_order_acquire) < expected &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count());
    metrics.total_sent = counters.sent.load();
    metrics.total_received = counters.received.load();
    metrics.total_dropped = 0;
    metrics.completed = shutdown.has_value() &&
                        metrics.total_sent == expected &&
                        metrics.total_received == expected;
    metrics.throughput_msgps = throughput(metrics.total_received,
                                          metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}
```

- [ ] **Step 5: Run mailbox test to verify GREEN**

Run:

```bash
ninja -C build test_bench_caf_mailbox_n1
./build/tests/integration/apps/test_bench_caf_mailbox_n1
```

Expected: `BenchCafMailboxN1.SmokeCompletesAllMessages` passes.

- [ ] **Step 6: Commit Task 4**

Run:

```bash
git add apps/bench_caf/actors/mailbox_n1_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp tests/integration/apps/CMakeLists.txt tests/integration/apps/test_bench_caf_mailbox_n1.cpp
git commit -m "feat(bench-caf): add mailbox n-to-one scenario"
```

---

### Task 5: Mixed Case Scenario

**Files:**
- Create: `apps/bench_caf/actors/mixed_case_actor.hpp`
- Create: `tests/integration/apps/test_bench_caf_mixed_case.cpp`
- Modify: `apps/bench_caf/caf_bench_scenarios.hpp`
- Modify: `tests/integration/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: `CafBenchConfig`, `TrialMetrics`, `make_bench_actor_config()`.
- Produces: `run_mixed_case_trial(const CafBenchConfig&, uint32_t)`.

- [ ] **Step 1: Write the failing mixed-case integration test**

Create `tests/integration/apps/test_bench_caf_mixed_case.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_scenarios.hpp>

#include <gtest/gtest.h>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafMixedCase, SmokeCompletesRingsAndCpuWork) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MixedCase;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.sample_rss_ms = 10;

    auto metrics = bench_caf::run_mixed_case_trial(cfg, 1);

    EXPECT_TRUE(metrics.completed);
    EXPECT_EQ(metrics.trial, 1u);
    EXPECT_EQ(metrics.rings_completed, 4u);
    EXPECT_EQ(metrics.cpu_tasks_completed, 4u);
    EXPECT_EQ(metrics.token_hops, 400u);
    EXPECT_GT(metrics.actors_created, 0u);
}
```

Modify `tests/integration/apps/CMakeLists.txt`:

```cmake
add_executable(test_bench_caf_mixed_case test_bench_caf_mixed_case.cpp)
target_include_directories(test_bench_caf_mixed_case PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_caf_mixed_case hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_caf_mixed_case PROPERTIES TIMEOUT 30)
```

- [ ] **Step 2: Run mixed-case test to verify RED**

Run:

```bash
ninja -C build test_bench_caf_mixed_case
```

Expected: compile fails because `run_mixed_case_trial()` is not defined.

- [ ] **Step 3: Add mixed-case actors**

Create `apps/bench_caf/actors/mixed_case_actor.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include "../messages.hpp"

#include <atomic>
#include <cstdint>

namespace hpactor::apps::bench_caf {

struct MixedCaseCounters {
    std::atomic<uint64_t> actors_created{0};
    std::atomic<uint64_t> rings_completed{0};
    std::atomic<uint64_t> token_hops{0};
    std::atomic<uint64_t> cpu_tasks_completed{0};
};

struct MixedCaseDimensions {
    uint32_t rings = 4;
    uint32_t ring_size = 16;
    uint32_t token_value = 100;
    uint32_t repetitions = 1;
};

inline MixedCaseDimensions mixed_case_dimensions_for_preset(PresetKind preset) {
    switch (preset) {
    case PresetKind::Smoke:
        return {4, 16, 100, 1};
    case PresetKind::Nightly:
        return {32, 64, 500, 2};
    case PresetKind::PaperScale:
        return {100, 100, 1000, 4};
    case PresetKind::Stress:
        return {128, 128, 2000, 4};
    }
    return {4, 16, 100, 1};
}

inline uint64_t factorization_work(uint64_t value) {
    uint64_t factors = 0;
    for (uint64_t candidate = 2; candidate * candidate <= value; ++candidate) {
        while (value % candidate == 0) {
            value /= candidate;
            ++factors;
        }
    }
    if (value > 1)
        ++factors;
    return factors;
}

class MixedCpuActor : public EventBasedActor {
  public:
    MixedCpuActor(ActorContext* ctx, ActorSystem& sys,
                  MixedCaseCounters* counters)
        : EventBasedActor(ctx, sys), counters_(counters) {
        counters_->actors_created.fetch_add(1, std::memory_order_relaxed);
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MixedCpuTaskTag)
                return;
            volatile auto factors = factorization_work(86028157ULL);
            (void)factors;
            counters_->cpu_tasks_completed.fetch_add(1,
                                                     std::memory_order_release);
            stop();
        }};
    }

  private:
    MixedCaseCounters* counters_ = nullptr;
};

class MixedRingNodeActor : public EventBasedActor {
  public:
    MixedRingNodeActor(ActorContext* ctx, ActorSystem& sys,
                       MixedCaseCounters* counters, ActorAddress next)
        : EventBasedActor(ctx, sys), counters_(counters), next_(next) {
        counters_->actors_created.fetch_add(1, std::memory_order_relaxed);
        become(make_behavior());
    }

    void set_next(ActorAddress next) {
        next_ = next;
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            if (msg.type_id() != MixedTokenTag)
                return;
            counters_->token_hops.fetch_add(1, std::memory_order_relaxed);
            auto remaining = decode_bench_payload(msg.payload()).sequence;
            if (remaining == 0) {
                counters_->rings_completed.fetch_add(1,
                                                     std::memory_order_release);
                return;
            }
            BenchPayloadHeader header;
            header.sequence = remaining - 1;
            context()->send(next_,
                            make_bench_msg(MixedTokenTag,
                                           encode_bench_payload(header, 0, 1)));
        }};
    }

  private:
    MixedCaseCounters* counters_ = nullptr;
    ActorAddress next_;
};

} // namespace hpactor::apps::bench_caf
```

- [ ] **Step 4: Add mixed-case scenario runner**

Modify `apps/bench_caf/caf_bench_scenarios.hpp`:

```cpp
#include "actors/mixed_case_actor.hpp"
```

Add below `run_mailbox_n1_trial()`:

```cpp
inline TrialMetrics run_mixed_case_trial(const CafBenchConfig& cfg,
                                         uint32_t trial_index) {
    TrialMetrics metrics;
    metrics.trial = trial_index;

    auto dims = mixed_case_dimensions_for_preset(cfg.preset);
    uint64_t expected_rings =
        static_cast<uint64_t>(dims.rings) * dims.repetitions;
    uint64_t expected_cpu = dims.rings;
    uint64_t expected_hops =
        static_cast<uint64_t>(dims.rings) * dims.repetitions *
        dims.token_value;

    MixedCaseCounters counters;
    RssSampler sampler(cfg.sample_rss_ms);
    auto start = std::chrono::steady_clock::now();
    sampler.start();

    ActorSystem system(make_bench_actor_config(cfg));

    for (uint32_t r = 0; r < dims.rings; ++r) {
        std::vector<ActorAddress> nodes;
        nodes.reserve(dims.ring_size);
        for (uint32_t i = 0; i < dims.ring_size; ++i) {
            auto node = system.spawn<MixedRingNodeActor>(&counters,
                                                        ActorAddress{});
            nodes.push_back(node.address());
        }

        for (uint32_t i = 0; i < dims.ring_size; ++i) {
            auto actor = std::static_pointer_cast<MixedRingNodeActor>(
                system.get_actor(nodes[i].id));
            actor->set_next(nodes[(i + 1) % dims.ring_size]);
        }

        auto cpu = system.spawn<MixedCpuActor>(&counters);
        system.deliver_local(cpu.id(), make_bench_msg(MixedCpuTaskTag));

        for (uint32_t rep = 0; rep < dims.repetitions; ++rep) {
            BenchPayloadHeader header;
            header.sequence = dims.token_value - 1;
            system.deliver_local(nodes[0].id,
                                 make_bench_msg(MixedTokenTag,
                                                encode_bench_payload(header,
                                                                     0, rep)));
        }
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while ((counters.rings_completed.load(std::memory_order_acquire) <
                expected_rings ||
            counters.cpu_tasks_completed.load(std::memory_order_acquire) <
                expected_cpu) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    auto shutdown = system.shutdown();
    auto end = std::chrono::steady_clock::now();
    auto samples = sampler.stop();

    metrics.runtime_ms = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(end - start)
            .count());
    metrics.actors_created = counters.actors_created.load();
    metrics.rings_completed = counters.rings_completed.load();
    metrics.token_hops = counters.token_hops.load();
    metrics.cpu_tasks_completed = counters.cpu_tasks_completed.load();
    metrics.completed = shutdown.has_value() &&
                        metrics.rings_completed == expected_rings &&
                        metrics.cpu_tasks_completed == expected_cpu &&
                        metrics.token_hops == expected_hops;
    metrics.throughput_msgps = throughput(metrics.token_hops,
                                          metrics.runtime_ms);
    metrics.peak_rss_bytes = peak_rss(samples);
    metrics.rss_samples_bytes = std::move(samples);
    return metrics;
}
```

- [ ] **Step 5: Run mixed-case test to verify GREEN**

Run:

```bash
ninja -C build test_bench_caf_mixed_case
./build/tests/integration/apps/test_bench_caf_mixed_case
```

Expected: `BenchCafMixedCase.SmokeCompletesRingsAndCpuWork` passes.

- [ ] **Step 6: Commit Task 5**

Run:

```bash
git add apps/bench_caf/actors/mixed_case_actor.hpp apps/bench_caf/caf_bench_scenarios.hpp tests/integration/apps/CMakeLists.txt tests/integration/apps/test_bench_caf_mixed_case.cpp
git commit -m "feat(bench-caf): add mixed case scenario"
```

---

### Task 6: Runner Dispatch, Trials, And Binary Smoke Tests

**Files:**
- Create: `apps/bench_caf/caf_bench_runner.hpp`
- Create: `tests/system/apps/test_bench_caf_smoke.cpp`
- Modify: `apps/bench_caf/18_bench_caf.cpp`
- Modify: `tests/system/apps/CMakeLists.txt`

**Interfaces:**
- Consumes: scenario entry points from Tasks 3-5, output functions from Task 1.
- Produces: `run_caf_benchmark(const CafBenchConfig&)`, binary smoke coverage for JSON and CSV output.

- [ ] **Step 1: Write failing runner and binary smoke tests**

Create `tests/system/apps/test_bench_caf_smoke.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include <apps/bench_caf/caf_bench_config.hpp>
#include <apps/bench_caf/caf_bench_runner.hpp>

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace bench_caf = hpactor::apps::bench_caf;

TEST(BenchCafRunner, RunsActorCreationTrial) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::ActorCreation;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.trials = 1;

    auto report = bench_caf::run_caf_benchmark(cfg);

    ASSERT_EQ(report.trials.size(), 1u);
    EXPECT_TRUE(report.trials[0].completed);
    EXPECT_EQ(report.trials[0].actors_created, 2047u);
}

TEST(BenchCafRunner, RunsMailboxTrial) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MailboxN1;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.trials = 1;

    auto report = bench_caf::run_caf_benchmark(cfg);

    ASSERT_EQ(report.trials.size(), 1u);
    EXPECT_TRUE(report.trials[0].completed);
    EXPECT_EQ(report.trials[0].total_sent, 40000u);
    EXPECT_EQ(report.trials[0].total_received, 40000u);
}

TEST(BenchCafRunner, RunsMixedCaseTrial) {
    bench_caf::CafBenchConfig cfg;
    cfg.scenario = bench_caf::ScenarioKind::MixedCase;
    cfg.preset = bench_caf::PresetKind::Smoke;
    cfg.scheduler_threads = 2;
    cfg.trials = 1;

    auto report = bench_caf::run_caf_benchmark(cfg);

    ASSERT_EQ(report.trials.size(), 1u);
    EXPECT_TRUE(report.trials[0].completed);
    EXPECT_EQ(report.trials[0].rings_completed, 4u);
}
```

Modify `tests/system/apps/CMakeLists.txt`:

```cmake
add_executable(test_bench_caf_smoke test_bench_caf_smoke.cpp)
target_include_directories(test_bench_caf_smoke PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_bench_caf_smoke hpactor hpactor_test_support GTest::gtest_main)
gtest_discover_tests(test_bench_caf_smoke PROPERTIES TIMEOUT 60)
```

- [ ] **Step 2: Run smoke test to verify RED**

Run:

```bash
ninja -C build test_bench_caf_smoke
```

Expected: compile fails because `caf_bench_runner.hpp` does not exist.

- [ ] **Step 3: Add runner dispatch**

Create `apps/bench_caf/caf_bench_runner.hpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#pragma once

#include "caf_bench_config.hpp"
#include "caf_bench_metrics.hpp"
#include "caf_bench_scenarios.hpp"

namespace hpactor::apps::bench_caf {

inline TrialMetrics run_one_trial(const CafBenchConfig& cfg,
                                  uint32_t trial_index) {
    switch (cfg.scenario) {
    case ScenarioKind::ActorCreation:
        return run_actor_creation_trial(cfg, trial_index);
    case ScenarioKind::MailboxN1:
        return run_mailbox_n1_trial(cfg, trial_index);
    case ScenarioKind::MixedCase:
        return run_mixed_case_trial(cfg, trial_index);
    }
    return run_actor_creation_trial(cfg, trial_index);
}

inline CafBenchReport run_caf_benchmark(const CafBenchConfig& cfg) {
    CafBenchReport report;
    report.config = cfg;

    for (uint32_t i = 0; i < cfg.warmups; ++i) {
        (void)run_one_trial(cfg, i + 1);
    }

    for (uint32_t i = 0; i < cfg.trials; ++i) {
        report.trials.push_back(run_one_trial(cfg, i + 1));
    }
    return report;
}

} // namespace hpactor::apps::bench_caf
```

- [ ] **Step 4: Wire runner into the binary**

Modify `apps/bench_caf/18_bench_caf.cpp`:

```cpp
// Copyright 2026 HPActor Contributors
//
// Licensed under the Apache License, Version 2.0 (the "License");

#include "caf_bench_config.hpp"
#include "caf_bench_output.hpp"
#include "caf_bench_runner.hpp"

#include <fstream>
#include <iostream>

namespace hpactor::apps::bench_caf {

int run_caf_benchmark_main(int argc, const char* const* argv) {
    auto parsed = parse_caf_bench_args(argc, argv);
    if (!parsed.ok) {
        std::cerr << parsed.error << '\n';
        return 1;
    }

    auto report = run_caf_benchmark(parsed.config);
    std::string output = parsed.config.format == OutputFormat::Csv
                             ? write_csv_report(report)
                             : write_json_report(report);

    if (!parsed.config.output_path.empty()) {
        std::ofstream out(parsed.config.output_path);
        if (!out) {
            std::cerr << "failed to open output path: "
                      << parsed.config.output_path << '\n';
            return 5;
        }
        out << output;
    } else {
        std::cout << output;
    }
    return 0;
}

} // namespace hpactor::apps::bench_caf

int main(int argc, const char* const* argv) {
    return hpactor::apps::bench_caf::run_caf_benchmark_main(argc, argv);
}
```

- [ ] **Step 5: Run smoke tests to verify GREEN**

Run:

```bash
ninja -C build 18_bench_caf test_bench_caf_smoke
./build/tests/system/apps/test_bench_caf_smoke
./build/apps/bench_caf/18_bench_caf --scenario actor-creation --preset smoke --format json --trials 1
./build/apps/bench_caf/18_bench_caf --scenario mailbox-n1 --preset smoke --format csv --trials 1
./build/apps/bench_caf/18_bench_caf --scenario mixed-case --preset smoke --format json --trials 1
```

Expected:

- `test_bench_caf_smoke` passes.
- The three binary invocations exit 0.
- JSON output contains `"benchmark": "caf-port"`.
- CSV output starts with `scenario,preset,trial,completed`.

- [ ] **Step 6: Commit Task 6**

Run:

```bash
git add apps/bench_caf/18_bench_caf.cpp apps/bench_caf/caf_bench_runner.hpp tests/system/apps/CMakeLists.txt tests/system/apps/test_bench_caf_smoke.cpp
git commit -m "feat(bench-caf): add runner dispatch and smoke coverage"
```

---

### Task 7: README, Final Verification, And Issue Linkage

**Files:**
- Create: `apps/bench_caf/README.md`
- Modify: `docs/superpowers/specs/2026-06-26-caf-performance-benchmark-design.md`

**Interfaces:**
- Consumes: completed `18_bench_caf` binary and Phase 1 scenarios.
- Produces: user-facing app README and final verification evidence.

- [ ] **Step 1: Add app README**

Create `apps/bench_caf/README.md`:

```markdown
# HPActor CAF Performance Benchmark

`18_bench_caf` ports CAF benchmark scenario contracts to HPActor-native actors.
It is intended for regression testing HPActor internals, not for copying CAF
implementation code.

## Phase 1 Scenarios

| Scenario | Purpose |
|----------|---------|
| `actor-creation` | Recursive actor spawn, fan-in, teardown, and allocator pressure. |
| `mailbox-n1` | Many producers sending to one receiver mailbox. |
| `mixed-case` | Ring token passing, actor lifecycle churn, and CPU work. |

## Examples

```bash
./build/apps/bench_caf/18_bench_caf --scenario actor-creation --preset smoke --format json
./build/apps/bench_caf/18_bench_caf --scenario mailbox-n1 --preset smoke --format csv
./build/apps/bench_caf/18_bench_caf --scenario mixed-case --preset smoke --trials 3
```

## Presets

`smoke` is suitable for quick regression checks. `nightly`, `paper-scale`, and
`stress` increase workload sizes and should be run in scheduled or manual
performance environments.
```

- [ ] **Step 2: Update the design spec status**

Modify the header of `docs/superpowers/specs/2026-06-26-caf-performance-benchmark-design.md`:

```markdown
**Status:** Phase 1 implementation planned
```

Add a short implementation-plan reference under the issue line:

```markdown
**Phase 1 plan:** `docs/superpowers/plans/2026-06-26-caf-performance-benchmark-phase1-impl.md`
```

- [ ] **Step 3: Run final targeted verification**

Run:

```bash
ninja -C build 18_bench_caf test_unit_apps test_bench_caf_actor_creation test_bench_caf_mailbox_n1 test_bench_caf_mixed_case test_bench_caf_smoke
./build/tests/unit/apps/test_unit_apps --gtest_filter="BenchCaf*"
./build/tests/integration/apps/test_bench_caf_actor_creation
./build/tests/integration/apps/test_bench_caf_mailbox_n1
./build/tests/integration/apps/test_bench_caf_mixed_case
./build/tests/system/apps/test_bench_caf_smoke
git diff --check
```

Expected:

- All selected Ninja targets build.
- All selected tests pass.
- `git diff --check` exits 0.

- [ ] **Step 4: Record no full-suite run**

In the final implementation response, state:

```text
I did not run the full C++ test suite; verification used targeted app/unit/integration/system tests for the new bench_caf surface.
```

- [ ] **Step 5: Commit Task 7**

Run:

```bash
git add apps/bench_caf/README.md docs/superpowers/specs/2026-06-26-caf-performance-benchmark-design.md
git commit -m "docs(bench-caf): document phase one benchmark app"
```

---

## Final Branch Verification

After all tasks are complete, run:

```bash
git status --short --branch
git log --oneline --decorate -8
git diff --check main...HEAD
ninja -C build 18_bench_caf test_unit_apps test_bench_caf_actor_creation test_bench_caf_mailbox_n1 test_bench_caf_mixed_case test_bench_caf_smoke
./build/tests/unit/apps/test_unit_apps --gtest_filter="BenchCaf*"
./build/tests/integration/apps/test_bench_caf_actor_creation
./build/tests/integration/apps/test_bench_caf_mailbox_n1
./build/tests/integration/apps/test_bench_caf_mixed_case
./build/tests/system/apps/test_bench_caf_smoke
```

Expected:

- Worktree is on `docs/caf-performance-benchmark-design` or the implementation
  branch chosen before execution.
- `git diff --check main...HEAD` exits 0.
- All selected targets build.
- All selected tests pass.
- `18_bench_caf` supports:
  - `--scenario actor-creation --preset smoke --format json`
  - `--scenario mailbox-n1 --preset smoke --format csv`
  - `--scenario mixed-case --preset smoke --format json`

## Phase 2 And Phase 3 Handoff

Create separate implementation plans after Phase 1 merges:

1. Phase 2: message size, message shape, and traffic distribution sweeps.
2. Phase 3: distributed ping/pong, dispatch matching, serialization, Mandelbrot,
   and scheduler-mix microbenchmarks.

Keeping those phases separate keeps Phase 1 reviewable and gives HPActor useful
CAF-style regression coverage early.
