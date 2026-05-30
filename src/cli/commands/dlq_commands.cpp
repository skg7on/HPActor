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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/cli/command_registry.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace hpactor {
namespace cli {
namespace {

// ── helpers ──────────────────────────────────────────────────────────────

std::string format_age_ns(uint64_t timestamp_ns) {
    if (timestamp_ns == 0)
        return "-";
    uint64_t now_ns = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
    uint64_t age_s = (now_ns - timestamp_ns) / 1'000'000'000ULL;
    if (age_s < 60)
        return std::to_string(age_s) + "s";
    if (age_s < 3600)
        return std::to_string(age_s / 60) + "m";
    return std::to_string(age_s / 3600) + "h";
}

std::string type_tag_hex(TypeTag tag) {
    std::stringstream ss;
    ss << "0x" << std::hex << std::setfill('0') << std::setw(4)
       << static_cast<uint16_t>(tag);
    return ss.str();
}

std::string trace_id_str(uint64_t hi, uint64_t lo) {
    if (hi == 0 && lo == 0)
        return "(none)";
    std::stringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << hi << std::setw(16)
       << lo;
    return ss.str();
}

// NOTE: ActorId uses Id<ActorIdTag, uint64_t>. Use .value() to get the
// uint64_t for display: std::to_string(id.value())

// ── /dlq list ────────────────────────────────────────────────────────────

class DlqListCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "dlq/list";
    }
    std::string_view help_text() const noexcept override {
        return "List dead-letter queue records";
    }
    int order() const noexcept override {
        return 500;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }

        auto records = dlq->snapshot_records();
        auto reason_filter = ctx.get_param("reason");
        auto source_filter = ctx.get_param("source");
        uint32_t limit_val = 50;
        if (auto lim = ctx.get_param("limit")) {
            limit_val = static_cast<uint32_t>(std::stoul(*lim));
        }

        std::vector<size_t> filtered;
        for (size_t i = 0; i < records.size(); ++i) {
            auto& r = records[i];
            if (reason_filter &&
                std::string(mailbox::to_string(r.reason)) != *reason_filter)
                continue;
            if (source_filter &&
                std::string(mailbox::to_string(r.source)) != *source_filter)
                continue;
            filtered.push_back(i);
        }

        ctx.output->header("Dead-Letter Queue Records (" +
                           std::to_string(filtered.size()) + " total)");

        std::vector<std::string> cols = {"#",      "Reason",  "Source",
                                         "Target", "TypeTag", "Age"};
        std::vector<std::vector<std::string>> rows;
        for (size_t j = 0; j < filtered.size() && j < limit_val; ++j) {
            size_t idx = filtered[j];
            auto& r = records[idx];
            rows.push_back({
                std::to_string(idx),
                std::string(mailbox::to_string(r.reason)),
                std::string(mailbox::to_string(r.source)),
                std::to_string(r.target.id.value()),
                type_tag_hex(r.type_tag),
                format_age_ns(r.timestamp_ns),
            });
        }
        ctx.output->table(cols, rows);

        if (filtered.size() > limit_val) {
            ctx.output->raw("... " + std::to_string(filtered.size() - limit_val) +
                            " more (use --limit to show more)");
        }
        return result<void>::make();
    }
};

// ── /dlq show --index N ──────────────────────────────────────────────────

class DlqShowCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "dlq/show";
    }
    std::string_view help_text() const noexcept override {
        return "Show a dead-letter record: /dlq show --index N";
    }
    int order() const noexcept override {
        return 510;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }
        auto idx_str = ctx.get_param("index");
        if (!idx_str) {
            ctx.output->error("Usage: /dlq show --index N");
            return result<void>::make();
        }
        size_t index = std::stoul(*idx_str);

        auto records = dlq->snapshot_records();
        if (index >= records.size()) {
            ctx.output->error("Index " + std::to_string(index) + " out of range (0.." +
                              std::to_string(records.size() - 1) + ")");
            return result<void>::make();
        }

        auto& r = records[index];
        std::map<std::string, std::string> kv;
        kv["Index"] = std::to_string(index);
        kv["Reason"] = std::string(mailbox::to_string(r.reason));
        kv["Source"] = std::string(mailbox::to_string(r.source));
        kv["Sender"] = std::to_string(r.sender.id.value());
        kv["Target"] = std::to_string(r.target.id.value());
        kv["TypeTag"] = type_tag_hex(r.type_tag);
        kv["MessageID"] = std::to_string(r.message_id);
        kv["Priority"] = std::to_string(r.priority);
        kv["Payload size"] = std::to_string(r.payload_size) + " bytes";
        kv["Mailbox"] = std::to_string(r.mailbox_depth) + "/" +
                        std::to_string(r.mailbox_capacity);
        kv["Age"] = format_age_ns(r.timestamp_ns);
        kv["TraceID"] = trace_id_str(r.trace_id_hi, r.trace_id_lo);

        ctx.output->header("Dead-Letter Record #" + std::to_string(index));
        ctx.output->key_value(kv);

        // Payload hex dump (first 128 bytes)
        if (r.payload_size > 0 && !r.payload_sample.empty()) {
            std::stringstream hex;
            const auto& buf = r.payload_sample;
            size_t show = std::min<size_t>(128, buf.size());
            for (size_t i = 0; i < show; ++i) {
                if (i > 0 && i % 16 == 0)
                    hex << "\n";
                hex << std::hex << std::setfill('0') << std::setw(2)
                    << static_cast<int>(buf[i]) << " ";
            }
            ctx.output->raw("Payload hex (first 128 bytes):");
            ctx.output->raw(hex.str());
        }

        // Failure envelope summary
        auto env = r.to_failure_envelope();
        std::map<std::string, std::string> env_kv;
        env_kv["FailureReason"] = std::string(to_string(env.reason));
        env_kv["FailureSource"] = std::string(to_string(env.source));
        env_kv["Retryable"] = env.retryable ? "yes" : "no";
        ctx.output->key_value(env_kv);

        return result<void>::make();
    }
};

// ── /dlq replay --index N ────────────────────────────────────────────────

class DlqReplayCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "dlq/replay";
    }
    std::string_view help_text() const noexcept override {
        return "Replay a dead-letter record: /dlq replay --index N";
    }
    int order() const noexcept override {
        return 520;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }
        auto idx_str = ctx.get_param("index");
        if (!idx_str) {
            ctx.output->error("Usage: /dlq replay --index N");
            return result<void>::make();
        }
        size_t index = std::stoul(*idx_str);

        mailbox::DeadLetterRecord r;
        if (!dlq->try_pop_at(index, r)) {
            ctx.output->error("Index " + std::to_string(index) + " out of range");
            return result<void>::make();
        }

        if (r.payload_sample.empty()) {
            ctx.output->error("Record has no payload - cannot replay");
            return result<void>::make();
        }

        // Reconstruct TypedMessage from stored payload and deliver.
        TypedMessage msg(r.type_tag, r.payload_sample);
        system->deliver_local(r.target.id, std::move(msg));
        ctx.output->raw("Replayed record #" + std::to_string(index) +
                        " to actor " + std::to_string(r.target.id.value()));
        return result<void>::make();
    }
};

// ── /dlq export ──────────────────────────────────────────────────────────

class DlqExportCommand final : public ICommand {
  public:
    std::string_view path() const noexcept override {
        return "dlq/export";
    }
    std::string_view help_text() const noexcept override {
        return "Export dead-letter records: /dlq export [--format json|text]";
    }
    int order() const noexcept override {
        return 530;
    }

    result<void> execute(CommandContext& ctx) const override {
        auto* system = ctx.system;
        if (!system) {
            ctx.output->error("No actor system available");
            return result<void>::make();
        }
        auto* dlq = system->dead_letter_queue();
        if (!dlq) {
            ctx.output->raw("Dead-letter queue is not enabled.");
            return result<void>::make();
        }

        auto records = dlq->snapshot_records();
        uint32_t limit_val = 100;
        if (auto lim = ctx.get_param("limit")) {
            limit_val = static_cast<uint32_t>(std::stoul(*lim));
        }
        bool json = ctx.get_param("format").value_or("text") == "json";

        if (json) {
            ctx.output->raw("[");
            for (size_t i = 0; i < records.size() && i < limit_val; ++i) {
                auto& r = records[i];
                std::stringstream ss;
                ss << "  {"
                   << R"("reason":")" << mailbox::to_string(r.reason) << "\","
                   << R"("source":")" << mailbox::to_string(r.source) << "\","
                   << R"("target":")" << r.target.id.value() << "\","
                   << R"("type_tag":")" << type_tag_hex(r.type_tag) << "\","
                   << R"("message_id":)" << r.message_id << ","
                   << R"("payload_size":)" << r.payload_size << "}";
                if (i + 1 < records.size() && i + 1 < limit_val)
                    ss << ",";
                ctx.output->raw(ss.str());
            }
            ctx.output->raw("]");
        } else {
            for (size_t i = 0; i < records.size() && i < limit_val; ++i) {
                auto& r = records[i];
                std::stringstream ss;
                ss << i << " " << mailbox::to_string(r.reason) << " "
                   << mailbox::to_string(r.source) << " " << r.target.id.value()
                   << " " << type_tag_hex(r.type_tag) << " " << r.payload_size
                   << "B";
                ctx.output->raw(ss.str());
            }
        }
        return result<void>::make();
    }
};

// ── registration ─────────────────────────────────────────────────────────

const CommandRegistration<DlqListCommand> kRegisterDlqList;
const CommandRegistration<DlqShowCommand> kRegisterDlqShow;
const CommandRegistration<DlqReplayCommand> kRegisterDlqReplay;
const CommandRegistration<DlqExportCommand> kRegisterDlqExport;

} // anonymous namespace
} // namespace cli
} // namespace hpactor
