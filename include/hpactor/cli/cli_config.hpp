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
#include <string>

namespace hpactor {
namespace cli {

struct CliConfig {
    bool enabled = false;  // CLI is opt-in — only enable via TOML or explicit config
    std::string listen_path;          // UDS path; empty = stdin/stdout
    uint16_t tcp_port = 0;           // TCP port; 0 = disabled
    std::string default_format = "pretty";
    uint32_t page_size = 50;
};

}  // namespace cli
}  // namespace hpactor
