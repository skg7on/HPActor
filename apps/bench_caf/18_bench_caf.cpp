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
            std::cerr << "failed to open output path: " << parsed.config.output_path
                      << '\n';
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
