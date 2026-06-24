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

#include <hpactor/cli/command/command_registry.hpp>
#include <string_view>

namespace hpactor::test {

inline cli::ICommand* find_cli_command(std::string_view path) {
    auto& reg = cli::CommandRegistry::instance();
    for (auto& c : reg.commands()) {
        if (c->path() == path)
            return c.get();
    }
    return nullptr;
}

} // namespace hpactor::test
