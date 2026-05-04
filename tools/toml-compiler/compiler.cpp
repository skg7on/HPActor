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

#include <hpactor/config/binary_serializer.hpp>
#include <hpactor/config/toml_parser.hpp>

#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>

using namespace hpactor::config;

int main(int argc, char* argv[]) {
    const char* input_path = nullptr;
    const char* output_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            input_path = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            output_path = argv[++i];
        }
    }

    if (!input_path || !output_path) {
        std::cerr << "Usage: hpactor_toml_compiler --input <main.toml> --output <topology.bin>\n";
        return 1;
    }

    // Parse TOML
    auto parse_result = TomlParser::parse(input_path);
    if (!parse_result.has_value()) {
        std::cerr << "Error: TOML parse failed\n";
        return 1;
    }

    // Serialize to binary
    auto binary = serialize_topology(parse_result.value());

    // Write output
    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        std::cerr << "Error: cannot write to " << output_path << "\n";
        return 1;
    }
    out.write(reinterpret_cast<const char*>(binary.data()),
              static_cast<std::streamsize>(binary.size()));
    out.close();

    std::cout << "Wrote " << binary.size() << " bytes to " << output_path << "\n";
    return 0;
}
