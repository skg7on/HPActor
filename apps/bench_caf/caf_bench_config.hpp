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

#include <cstdint>
#include <cstdlib>
#include <string>

namespace hpactor::apps::bench_caf {

enum class ScenarioKind {
    ActorCreation,
    MailboxN1,
    MixedCase,
    TrafficOneToOne,
    TrafficOneToN,
    TrafficNToNRandom,
    TrafficRing,
    TrafficPipeline,
    TrafficZipf,
    TrafficBursty,
    MessageCreation,
    DispatchMatch,
    Serialization,
    Mandelbrot,
    SchedulingMix,
    DistributedPing,
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
    ProtobufSmall,
    ProtobufNested,
    SharedBuffer,
    Mixed80_20,
};

enum class TrafficDistribution {
    NToOne,
    OneToOne,
    OneToN,
    NToNRandom,
    Ring,
    Pipeline,
    ZipfHotspot,
    BurstyWaves,
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
    bool scheduler_start_paused = false;
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
        case ScenarioKind::TrafficOneToOne:
            return "traffic-one-to-one";
        case ScenarioKind::TrafficOneToN:
            return "traffic-one-to-n";
        case ScenarioKind::TrafficNToNRandom:
            return "traffic-n-to-n-random";
        case ScenarioKind::TrafficRing:
            return "traffic-ring";
        case ScenarioKind::TrafficPipeline:
            return "traffic-pipeline";
        case ScenarioKind::TrafficZipf:
            return "traffic-zipf";
        case ScenarioKind::TrafficBursty:
            return "traffic-bursty";
        case ScenarioKind::MessageCreation:
            return "message-creation";
        case ScenarioKind::DispatchMatch:
            return "dispatch-match";
        case ScenarioKind::Serialization:
            return "serialization";
        case ScenarioKind::Mandelbrot:
            return "mandelbrot";
        case ScenarioKind::SchedulingMix:
            return "scheduling-mix";
        case ScenarioKind::DistributedPing:
            return "distributed-ping";
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
    switch (shape) {
        case MessageShape::HeaderOnly:
            return "header-only";
        case MessageShape::FixedBytes:
            return "fixed-bytes";
        case MessageShape::ProtobufSmall:
            return "protobuf-small";
        case MessageShape::ProtobufNested:
            return "protobuf-nested";
        case MessageShape::SharedBuffer:
            return "shared-buffer";
        case MessageShape::Mixed80_20:
            return "mixed-80-20";
    }
    return "header-only";
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
    if (value == "traffic-one-to-one") {
        out = ScenarioKind::TrafficOneToOne;
        return true;
    }
    if (value == "traffic-one-to-n") {
        out = ScenarioKind::TrafficOneToN;
        return true;
    }
    if (value == "traffic-n-to-n-random") {
        out = ScenarioKind::TrafficNToNRandom;
        return true;
    }
    if (value == "traffic-ring") {
        out = ScenarioKind::TrafficRing;
        return true;
    }
    if (value == "traffic-pipeline") {
        out = ScenarioKind::TrafficPipeline;
        return true;
    }
    if (value == "traffic-zipf") {
        out = ScenarioKind::TrafficZipf;
        return true;
    }
    if (value == "traffic-bursty") {
        out = ScenarioKind::TrafficBursty;
        return true;
    }
    if (value == "message-creation") {
        out = ScenarioKind::MessageCreation;
        return true;
    }
    if (value == "dispatch-match") {
        out = ScenarioKind::DispatchMatch;
        return true;
    }
    if (value == "serialization") {
        out = ScenarioKind::Serialization;
        return true;
    }
    if (value == "mandelbrot") {
        out = ScenarioKind::Mandelbrot;
        return true;
    }
    if (value == "scheduling-mix") {
        out = ScenarioKind::SchedulingMix;
        return true;
    }
    if (value == "distributed-ping") {
        out = ScenarioKind::DistributedPing;
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
    if (value == "protobuf-small") {
        out = MessageShape::ProtobufSmall;
        return true;
    }
    if (value == "protobuf-nested") {
        out = MessageShape::ProtobufNested;
        return true;
    }
    if (value == "shared-buffer") {
        out = MessageShape::SharedBuffer;
        return true;
    }
    if (value == "mixed-80-20") {
        out = MessageShape::Mixed80_20;
        return true;
    }
    return false;
}

inline const char* distribution_name(TrafficDistribution dist) {
    switch (dist) {
        case TrafficDistribution::NToOne:
            return "n-to-one";
        case TrafficDistribution::OneToOne:
            return "one-to-one";
        case TrafficDistribution::OneToN:
            return "one-to-n";
        case TrafficDistribution::NToNRandom:
            return "n-to-n-random";
        case TrafficDistribution::Ring:
            return "ring";
        case TrafficDistribution::Pipeline:
            return "pipeline";
        case TrafficDistribution::ZipfHotspot:
            return "zipf-hotspot";
        case TrafficDistribution::BurstyWaves:
            return "bursty-waves";
    }
    return "n-to-one";
}

inline bool parse_distribution(const std::string& value, TrafficDistribution& out) {
    if (value == "n-to-one") {
        out = TrafficDistribution::NToOne;
        return true;
    }
    if (value == "one-to-one") {
        out = TrafficDistribution::OneToOne;
        return true;
    }
    if (value == "one-to-n") {
        out = TrafficDistribution::OneToN;
        return true;
    }
    if (value == "n-to-n-random") {
        out = TrafficDistribution::NToNRandom;
        return true;
    }
    if (value == "ring") {
        out = TrafficDistribution::Ring;
        return true;
    }
    if (value == "pipeline") {
        out = TrafficDistribution::Pipeline;
        return true;
    }
    if (value == "zipf-hotspot") {
        out = TrafficDistribution::ZipfHotspot;
        return true;
    }
    if (value == "bursty-waves") {
        out = TrafficDistribution::BurstyWaves;
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
        auto require_value = [&]() -> const char* {
            if (i + 1 >= argc)
                return nullptr;
            ++i;
            return argv[i];
        };

        if (arg == "--scenario") {
            const char* value = require_value();
            if (!value || !parse_scenario(value, cfg.scenario))
                return {false, cfg, "invalid --scenario"};
        } else if (arg == "--preset") {
            const char* value = require_value();
            if (!value || !parse_preset(value, cfg.preset))
                return {false, cfg, "invalid --preset"};
        } else if (arg == "--format") {
            const char* value = require_value();
            if (!value || !parse_format(value, cfg.format))
                return {false, cfg, "invalid --format"};
        } else if (arg == "--message-shape") {
            const char* value = require_value();
            if (!value || !parse_message_shape(value, cfg.message_shape))
                return {false, cfg, "invalid --message-shape"};
        } else if (arg == "--scheduler-threads") {
            const char* value = require_value();
            if (!value || !parse_u32(value, cfg.scheduler_threads))
                return {false, cfg, "invalid --scheduler-threads"};
        } else if (arg == "--trials") {
            const char* value = require_value();
            if (!value || !parse_u32(value, cfg.trials))
                return {false, cfg, "invalid --trials"};
        } else if (arg == "--warmup") {
            const char* value = require_value();
            if (!value || !parse_u32(value, cfg.warmups))
                return {false, cfg, "invalid --warmup"};
        } else if (arg == "--message-size") {
            const char* value = require_value();
            if (!value || !parse_u32(value, cfg.message_size_bytes))
                return {false, cfg, "invalid --message-size"};
        } else if (arg == "--mailbox-capacity") {
            const char* value = require_value();
            if (!value || !parse_u32(value, cfg.mailbox_capacity))
                return {false, cfg, "invalid --mailbox-capacity"};
        } else if (arg == "--seed") {
            const char* value = require_value();
            if (!value || !parse_u64(value, cfg.seed))
                return {false, cfg, "invalid --seed"};
        } else if (arg == "--sample-rss-ms") {
            const char* value = require_value();
            if (!value || !parse_u32(value, cfg.sample_rss_ms))
                return {false, cfg, "invalid --sample-rss-ms"};
        } else if (arg == "--output") {
            const char* value = require_value();
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
