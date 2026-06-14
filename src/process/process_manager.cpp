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

#include <cctype>
#include <hpactor/process/process_config.hpp>
#include <string>

namespace hpactor::process {

ProcessMode ProcessConfig::parse_mode(const std::string& s) {
    std::string lower;
    lower.reserve(s.size());
    for (char c : s)
        lower.push_back(
            static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lower == "systemd")
        return ProcessMode::Systemd;
    if (lower == "daemon")
        return ProcessMode::Daemon;
    return ProcessMode::Foreground;
}

} // namespace hpactor::process
