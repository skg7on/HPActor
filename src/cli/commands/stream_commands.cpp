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

#include <hpactor/cli/command/command_registry.hpp>
#include <hpactor/cli/format/output_formatter.hpp>

#include <map>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {
namespace {

class StreamListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "stream/list";
    }
    std::string_view help_text() const noexcept override {
        return "List active stream sessions";
    }
    int order() const noexcept override {
        return 600;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Active Streams");
        std::vector<std::string> cols = {"Stream ID", "Sender", "Receiver",
                                         "State",     "Window", "In Flight"};
        std::vector<std::vector<std::string>> rows;
        // In the future, enumerate from ActorSystem stream registry
        ctx.output->table(cols, rows);
        return result<void>::make();
    }
};

class StreamShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "stream/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show detailed stream state: /stream show <stream_id>";
    }
    int order() const noexcept override {
        return 601;
    }

    result<void> execute(CommandContext& ctx) const override {
        ctx.output->header("Stream Details");
        std::map<std::string, std::string> kv;
        kv["Note"] = "Stream inspection coming in follow-up";
        ctx.output->key_value(kv);
        return result<void>::make();
    }
};

const CommandRegistration<StreamListCommand> kRegStreamList;
const CommandRegistration<StreamShowCommand> kRegStreamShow;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
