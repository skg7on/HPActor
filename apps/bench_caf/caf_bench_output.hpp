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
    out << "  \"scenario\": \"" << scenario_name(report.config.scenario) << "\",\n";
    out << "  \"preset\": \"" << preset_name(report.config.preset) << "\",\n";
    out << "  \"runtime\": {\n";
    out << "    \"scheduler_threads\": " << report.config.scheduler_threads << ",\n";
    out << "    \"mailbox_capacity\": " << report.config.mailbox_capacity << "\n";
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
        out << "      \"cpu_tasks_completed\": " << t.cpu_tasks_completed << ",\n";
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
            << t.total_sent << ',' << t.total_received << ',' << t.total_rejected
            << ',' << t.total_dropped << ',' << t.actors_created << ','
            << t.actors_completed << ',' << t.rings_completed << ','
            << t.token_hops << ',' << t.cpu_tasks_completed << ',' << std::fixed
            << std::setprecision(3) << t.throughput_msgps << ','
            << t.peak_rss_bytes << '\n';
    }
    return out.str();
}

} // namespace hpactor::apps::bench_caf
