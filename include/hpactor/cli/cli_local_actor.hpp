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

#include <hpactor/cli/cli_config.hpp>
#include <hpactor/cli/cli_types.hpp>
#include <hpactor/cli/interactive_cli_actor.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/cli/pager.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hpactor {

class ActorSystem;

namespace cli {

// Forward-declare protobuf types (defined in cli_messages.pb.h)
class InspectStateReply;
class KillReply;
class QuarantineReply;

/// \brief Interactive CLI actor reading from stdin.
///
/// Sends actor requests via \c try_deliver_local() and polls its mailbox
/// for replies. Runs on a dedicated daemon thread via the
/// \c InteractiveCliActor Template Method lifecycle.
class CliActor : public InteractiveCliActor {
  public:
    static constexpr const char* kActorTypeName = "CliActor";

    cli::ActorMeta to_metadata() const override {
        cli::ActorMeta m;
        m.actor_id = id().value();
        m.actor_type = std::string(type_name());
        m.state = running_ ? "Running" : "Stopped";
        return m;
    }

    CliActor(ActorContext* ctx, ActorSystem& system, const CliConfig& config);
    ~CliActor() override;

    // CliActor-specific accessors
    const CliConfig& config() const {
        return config_;
    }
    OutputFormatter* formatter() {
        return formatter_.get();
    }
    Pager* pager() {
        return pager_.get();
    }

    // ICliCommandHost — local dispatch via try_deliver_local + mailbox poll
    std::optional<InspectStateReply>
    inspect(ActorId target, const InspectStateRequest& req,
            std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<KillReply>
    kill(ActorId target, const KillRequest& req,
         std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::optional<QuarantineReply>
    quarantine(ActorId target, const QuarantineRequest& req,
               std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) override;
    std::vector<ActorMeta> enumerate(std::string_view filter = "") override;

    // ISystemCliHost — render from local ActorSystem data
    void render_system_stats(OutputFormatter& output) override;
    void render_memory_stats(OutputFormatter& output) override;
    void render_fault_status(OutputFormatter& output) override;
    void render_scheduler_workers(OutputFormatter& output) override;
    void render_metrics_show(OutputFormatter& output) override;
    void render_dlq_list(OutputFormatter& output,
                         std::string_view filter = "") override;
    result<void> dlq_replay(uint32_t index, ActorId target) override;

    // ILifecycleCliHost
    result<void> drain() override;
    result<void> shutdown() override;

    static std::string get_history_path(const CliConfig& config);

    InspectStateReply
    build_self_inspect_reply(const class InspectStateRequest& req);

  protected:
    // InteractiveCliActor virtual hooks
    void print_greeting() override;
    void print_farewell() override;
    std::string get_history_path() override;
    uint32_t get_history_max() override;
    std::string get_default_format() override;
    uint32_t get_page_size() override;

  private:
    std::optional<StreamBuffer>
    poll_for_response(TypeTag expected_tag, std::chrono::milliseconds timeout);

    CliConfig config_;
    std::unique_ptr<OutputFormatter> formatter_;
    std::unique_ptr<Pager> pager_;
};

} // namespace cli
} // namespace hpactor
