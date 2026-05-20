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

#include <hpactor/cli/command_context.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor {
namespace cli {

// Abstract command registered via static file-scope registrar.
// Path uses "/" separators; segments in <angle brackets> mark parameter nodes.
// Examples: "help", "actor/<id>/show", "system/drain/status"
class ICommand {
  public:
    virtual ~ICommand() = default;
    virtual std::string_view path() const noexcept = 0;
    virtual std::string_view help_text() const noexcept = 0;
    virtual int order() const noexcept = 0;
    virtual result<void> execute(CommandContext& ctx) const = 0;
};

class CommandRegistry {
  public:
    static CommandRegistry& instance();

    void add(std::unique_ptr<ICommand> cmd);
    const std::vector<std::unique_ptr<ICommand>>& commands() const;

  private:
    CommandRegistry() = default;
    std::vector<std::unique_ptr<ICommand>> commands_;
};

template <typename CommandT> class CommandRegistration {
  public:
    CommandRegistration() {
        CommandRegistry::instance().add(std::make_unique<CommandT>());
    }
};

// Parse a path string into segments. "<id>" is a param, "actor" is literal.
inline std::vector<std::string> parse_command_path(std::string_view path) {
    std::vector<std::string> segs;
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string_view seg = (slash == std::string_view::npos)
                                   ? path.substr(start)
                                   : path.substr(start, slash - start);
        if (!seg.empty())
            segs.emplace_back(seg);
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return segs;
}

// Returns true if segment is a parameter placeholder, e.g. "<id>", "<actor_id>"
inline bool is_param_segment(const std::string& seg) {
    return !seg.empty() && seg.front() == '<' && seg.back() == '>';
}

} // namespace cli
} // namespace hpactor
