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

#include <gtest/gtest.h>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pretty_formatter.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/types/types.hpp>

namespace {

class MockCommandHost : public hpactor::cli::ICliCommandHost {
  public:
    int inspect_calls = 0;
    int kill_calls = 0;
    int enumerate_calls = 0;

    std::optional<hpactor::cli::InspectStateReply>
    inspect(hpactor::ActorId, const hpactor::cli::InspectStateRequest&,
            std::chrono::milliseconds) override {
        inspect_calls++;
        hpactor::cli::InspectStateReply reply;
        reply.mutable_metadata()->set_actor_id(42);
        reply.mutable_metadata()->set_actor_type("TestActor");
        return reply;
    }

    std::optional<hpactor::cli::KillReply>
    kill(hpactor::ActorId, const hpactor::cli::KillRequest&,
         std::chrono::milliseconds) override {
        kill_calls++;
        hpactor::cli::KillReply reply;
        reply.set_success(true);
        return reply;
    }

    std::optional<hpactor::cli::QuarantineReply>
    quarantine(hpactor::ActorId, const hpactor::cli::QuarantineRequest&,
               std::chrono::milliseconds) override {
        hpactor::cli::QuarantineReply reply;
        reply.set_success(true);
        return reply;
    }

    std::vector<hpactor::cli::ActorMeta> enumerate(std::string_view) override {
        enumerate_calls++;
        hpactor::cli::ActorMeta m;
        m.actor_id = 1;
        m.actor_type = "TestActor";
        return {m};
    }
};

class MockSystemHost : public hpactor::cli::ISystemCliHost {
  public:
    std::string last_output;

    void render_system_stats(hpactor::cli::OutputFormatter& output) override {
        output.header("Stats");
        last_output = "stats_rendered";
    }
    void render_memory_stats(hpactor::cli::OutputFormatter& output) override {
        output.header("Memory");
        last_output = "memory_rendered";
    }
    void render_fault_status(hpactor::cli::OutputFormatter& output) override {
        output.header("Faults");
        last_output = "faults_rendered";
    }
    void render_dlq_list(hpactor::cli::OutputFormatter& output,
                         std::string_view filter) override {
        output.header("DLQ");
        last_output = filter.empty() ? "dlq_all" : std::string(filter);
    }
    hpactor::result<void> dlq_replay(uint32_t, hpactor::ActorId) override {
        last_output = "dlq_replayed";
        return hpactor::result<void>::make();
    }
};

class MockLifecycleHost : public hpactor::cli::ILifecycleCliHost {
  public:
    bool drained = false;
    bool shut_down = false;

    hpactor::result<void> drain() override {
        drained = true;
        return hpactor::result<void>::make();
    }
    hpactor::result<void> shutdown() override {
        shut_down = true;
        return hpactor::result<void>::make();
    }
};

} // anonymous namespace

TEST(CliCommandHost, InspectDelegatesToMock) {
    MockCommandHost host;
    hpactor::cli::InspectStateRequest req;
    auto result =
        host.inspect(hpactor::ActorId{42}, req, std::chrono::milliseconds(2000));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->metadata().actor_id(), 42u);
    EXPECT_EQ(result->metadata().actor_type(), "TestActor");
    EXPECT_EQ(host.inspect_calls, 1);
}

TEST(CliCommandHost, KillDelegatesToMock) {
    MockCommandHost host;
    hpactor::cli::KillRequest req;
    auto result =
        host.kill(hpactor::ActorId{1}, req, std::chrono::milliseconds(2000));
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->success());
    EXPECT_EQ(host.kill_calls, 1);
}

TEST(CliCommandHost, EnumerateDelegatesToMock) {
    MockCommandHost host;
    auto actors = host.enumerate("");
    ASSERT_EQ(actors.size(), 1u);
    EXPECT_EQ(actors[0].actor_id, 1u);
    EXPECT_EQ(host.enumerate_calls, 1);
}

TEST(CliSystemHost, RenderSystemStatsDelegatesToMock) {
    MockSystemHost host;
    hpactor::cli::PrettyFormatter fmt;
    host.render_system_stats(fmt);
    EXPECT_EQ(host.last_output, "stats_rendered");
}

TEST(CliSystemHost, DlqReplayDelegatesToMock) {
    MockSystemHost host;
    auto result = host.dlq_replay(0, hpactor::ActorId{1});
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(host.last_output, "dlq_replayed");
}

TEST(CliLifecycleHost, DrainDelegatesToMock) {
    MockLifecycleHost host;
    auto result = host.drain();
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(host.drained);
}

TEST(CliLifecycleHost, ShutdownDelegatesToMock) {
    MockLifecycleHost host;
    auto result = host.shutdown();
    EXPECT_TRUE(result.has_value());
    EXPECT_TRUE(host.shut_down);
}
