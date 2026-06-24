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

#include <hpactor/cli/command/cli_session.hpp>
#include <hpactor/cli/command/command_context.hpp>
#include <hpactor/cli/command/command_node.hpp>
#include <hpactor/cli/io/lexer.hpp>
#include <hpactor/cli/format/output_formatter.hpp>
#include <hpactor/cli/io/pager.hpp>
#include <hpactor/cli/token.hpp>
#include <hpactor/fault/fault_macros.hpp>

namespace hpactor {
namespace cli {

CliSession::CliSession(ActorSystem* system, const CommandNode* command_tree,
                       std::unique_ptr<OutputFormatter> formatter,
                       std::function<void(const std::string&)> output_fn,
                       uint32_t page_size)
    : system_(system), command_tree_(command_tree),
      formatter_(std::move(formatter)),
      pager_(std::make_unique<Pager>(page_size)),
      output_fn_(std::move(output_fn)), page_size_(page_size) {}

CliSession::~CliSession() = default;

void CliSession::request_shutdown() {
    keep_running_ = false;
}

bool CliSession::process_line(const std::string& line) {
    if (!keep_running_)
        return false;
    if (line.empty())
        return true;

    formatter_ = OutputFormatter::create(current_format_);
    auto tokens = Lexer::tokenize(line);
    execute_tokens(tokens);
    return keep_running_;
}

void CliSession::execute_tokens(const std::vector<Token>& tokens) {
    FAULT_INJECT("hpactor.cli.execute_tokens.corrupt") {
        return;
    }

    CommandContext ctx;
    ctx.system = system_;
    ctx.cli_session = this;
    ctx.cli_actor = cli_actor_;
    ctx.cli_server_actor = cli_server_actor_;
    ctx.cli_proto_server = proto_server_;
    ctx.cli_client_actor = client_actor_;
    ctx.command_host = command_host_;
    ctx.system_host = system_host_;
    ctx.lifecycle_host = lifecycle_host_;
    ctx.output = formatter_.get();
    ctx.page_size = page_size_;

    CommandNode* node = const_cast<CommandNode*>(command_tree_);

    size_t i = 0;
    if (i < tokens.size() && tokens[i].value == "/") {
        ++i;
    }

    for (; i < tokens.size(); ++i) {
        auto& tok = tokens[i];

        if (tok.type == TokenType::Eof) {
            break;
        }

        if (tok.type == TokenType::Flag) {
            ctx.params[tok.value] = "true";
            continue;
        }

        if (tok.type == TokenType::FlagWithArg) {
            ctx.params[tok.value] = tok.arg.value_or("true");
            if (tok.value == "format") {
                current_format_ = tok.arg.value_or("pretty");
                formatter_ = OutputFormatter::create(current_format_);
                ctx.output = formatter_.get();
            }
            continue;
        }

        std::string param_value;
        auto* child = node->find_child(tok.value, param_value);
        if (!child) {
            if (node->execute) {
                ctx.args.push_back(tok.value);
                for (++i; i < tokens.size(); ++i) {
                    if (tokens[i].type == TokenType::Eof)
                        break;
                    if (tokens[i].type == TokenType::Flag) {
                        ctx.params[tokens[i].value] = "true";
                        continue;
                    }
                    if (tokens[i].type == TokenType::FlagWithArg) {
                        ctx.params[tokens[i].value] =
                            tokens[i].arg.value_or("true");
                        continue;
                    }
                    ctx.args.push_back(tokens[i].value);
                }
                break;
            }
            auto suggestion = node->suggest(tok.value);
            std::string err = "Unknown command '" + tok.value + "'";
            if (!suggestion.empty()) {
                err += " \xe2\x80\x94 did you mean '" + suggestion + "'?";
            }
            formatter_->error(err);
            output_fn_(formatter_->finalize() + "\n");
            return;
        }

        if (child->is_parameter) {
            ctx.params[child->keyword] = param_value;
        }
        node = child;
    }

    if (node->execute) {
        node->execute(ctx);
    } else if (!node->children.empty()) {
        formatter_->header("Available commands");
        formatter_->raw(node->help());
    }

    output_fn_(formatter_->finalize() + "\n");
}

} // namespace cli
} // namespace hpactor
