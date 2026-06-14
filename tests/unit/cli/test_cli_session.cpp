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

#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_context.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/output_formatter.hpp>

#include <gtest/gtest.h>

#include <string>

using namespace hpactor::cli;
using hpactor::result;

namespace {

class CliSessionTest : public ::testing::Test {
  protected:
    void SetUp() override {
        root_ = std::make_unique<CommandNode>("/", "root");
        auto* hello = root_->add_child("hello", "Say hello");
        hello->execute = [](CommandContext& ctx) -> result<void> {
            ctx.output->raw("Hello, world!");
            return result<void>::make();
        };
    }

    std::unique_ptr<CommandNode> root_;
};

} // anonymous namespace

TEST_F(CliSessionTest, ProcessHelloCommand) {
    std::string output;
    auto output_fn = [&](const std::string& s) { output += s; };
    auto formatter = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(formatter), output_fn);

    bool keep_going = session.process_line("/hello");
    EXPECT_TRUE(keep_going);
    EXPECT_NE(output.find("Hello, world!"), std::string::npos);
}

TEST_F(CliSessionTest, EmptyLineIsNoOp) {
    std::string output;
    auto output_fn = [&](const std::string& s) { output += s; };
    auto formatter = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(formatter), output_fn);

    bool keep_going = session.process_line("");
    EXPECT_TRUE(keep_going);
    EXPECT_TRUE(output.empty());
}

TEST_F(CliSessionTest, UnknownCommandShowsError) {
    std::string output;
    auto output_fn = [&](const std::string& s) { output += s; };
    auto formatter = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(formatter), output_fn);

    bool keep_going = session.process_line("/bogus");
    EXPECT_TRUE(keep_going);
    EXPECT_NE(output.find("Unknown command"), std::string::npos);
}

TEST_F(CliSessionTest, RequestShutdownStopsProcessing) {
    std::string output;
    auto output_fn = [&](const std::string& s) { output += s; };
    auto formatter = OutputFormatter::create("pretty");
    CliSession session(nullptr, root_.get(), std::move(formatter), output_fn);

    session.request_shutdown();
    bool keep_going = session.process_line("/hello");
    EXPECT_FALSE(keep_going);
    EXPECT_TRUE(output.empty());
}
