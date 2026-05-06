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

#include <hpactor/types/types.hpp>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

class CliActor;
class OutputFormatter;

struct CommandContext {
    std::vector<std::string> args;
    std::map<std::string, std::string> params;
    ActorSystem* system = nullptr;
    CliActor* cli_actor = nullptr;
    OutputFormatter* output = nullptr;
    bool paged = false;
    uint32_t page_size = 50;
    std::string format = "pretty";

    bool has_flag(const std::string& name) const {
        auto it = params.find(name);
        return it != params.end() && (it->second == "true" || it->second.empty());
    }

    std::optional<std::string> get_param(const std::string& name) const {
        auto it = params.find(name);
        if (it != params.end()) return it->second;
        return std::nullopt;
    }
};

}  // namespace cli
}  // namespace hpactor
