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

#include <hpactor/cli/cli_http_server_actor.hpp>

#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli.pb.h>

#include <hpactor/cli/cli_messages.pb.h>
#include <hpactor/cli/cli_session.hpp>
#include <hpactor/cli/command_node.hpp>
#include <hpactor/cli/command_tree_builder.hpp>
#include <hpactor/cli/output_formatter.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_gateway.hpp>
#include <hpactor/net/http_types.hpp>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace hpactor {
namespace cli {

// ---------------------------------------------------------------------------
// Minimal JSON helpers (no protobuf JSON util dependency)
// ---------------------------------------------------------------------------

/// Alias ADT helpers into the anonymous namespace so the protobuf-aware
/// parse/serialize functions below can call them without qualification.
namespace {

using adt::extract_json_array_raw;
using adt::extract_json_object_raw;
using adt::extract_json_string;
using adt::json_escape;
using adt::parse_json_string_array;
using adt::parse_json_string_map;
using adt::skip_json_ws;

/// Parse a JSON object to populate a CliCommand's fields.
/// Returns true on success, false on parse failure.
bool parse_cli_command_json(const std::string& json, hpactor::cli::CliCommand& cmd) {
    size_t pos = 0;
    pos = skip_json_ws(json, pos);
    if (pos >= json.size() || json[pos] != '{')
        return false;
    ++pos;

    std::string current_key;
    while (pos < json.size()) {
        pos = skip_json_ws(json, pos);
        if (pos >= json.size())
            break;
        if (json[pos] == '}') {
            ++pos;
            break;
        }
        if (json[pos] == ',') {
            ++pos;
            current_key.clear();
            continue;
        }
        if (json[pos] == '"' && current_key.empty()) {
            // Reading a key
            current_key = extract_json_string(json, pos);
            pos = skip_json_ws(json, pos);
            if (pos < json.size() && json[pos] == ':')
                ++pos;
            pos = skip_json_ws(json, pos);
            if (pos >= json.size())
                break;

            if (current_key == "path") {
                if (json[pos] == '"') {
                    cmd.set_path(extract_json_string(json, pos));
                }
                current_key.clear();
            } else if (current_key == "format") {
                if (json[pos] == '"') {
                    cmd.set_format(extract_json_string(json, pos));
                }
                current_key.clear();
            } else if (current_key == "params") {
                if (json[pos] == '{') {
                    auto obj_raw = extract_json_object_raw(json, pos);
                    auto pairs = parse_json_string_map(obj_raw);
                    auto* params_map = cmd.mutable_params();
                    for (auto& [k, v] : pairs) {
                        (*params_map)[k] = v;
                    }
                }
                current_key.clear();
            } else if (current_key == "args") {
                if (json[pos] == '[') {
                    auto arr_raw = extract_json_array_raw(json, pos);
                    auto values = parse_json_string_array(arr_raw);
                    for (auto& v : values) {
                        cmd.add_args(std::move(v));
                    }
                }
                current_key.clear();
            } else {
                // Skip unknown field
                if (json[pos] == '"') {
                    skip_json_ws(json, pos); // dummy — extract_json_string
                                             // advances
                    // Reset pos to where we were, then skip the value
                }
                // Skip the value (string, object, array, or literal)
                if (pos < json.size() && json[pos] == '"') {
                    extract_json_string(json, pos);
                } else if (pos < json.size() && json[pos] == '{') {
                    extract_json_object_raw(json, pos);
                } else if (pos < json.size() && json[pos] == '[') {
                    extract_json_array_raw(json, pos);
                } else {
                    // Skip literal (number, true, false, null)
                    while (pos < json.size() && json[pos] != ',' &&
                           json[pos] != '}') {
                        ++pos;
                    }
                }
                current_key.clear();
            }
        } else {
            // Unexpected; skip one character
            ++pos;
        }
    }
    return true;
}

/// Serialize a CliResponse to a JSON string.
std::string serialize_cli_response_json(const hpactor::cli::CliResponse& resp) {
    std::string json;
    json = "{";
    json += "\"content_type\":\"" + json_escape(resp.content_type()) + "\",";
    json += "\"payload\":\"" + json_escape(resp.payload()) + "\",";
    json += "\"is_error\":" + std::string(resp.is_error() ? "true" : "false") + ",";
    json += "\"error_code\":" + std::to_string(resp.error_code()) + ",";
    json += "\"is_structured\":" +
            std::string(resp.is_structured() ? "true" : "false");
    json += "}";
    return json;
}

/// Send a JSON CliResponse as an HTTP response.
void send_json_response(net::HTTPConnection* conn, net::HttpStatusCode http_code,
                        const hpactor::cli::CliResponse& resp) {
    std::string json_out = serialize_cli_response_json(resp);
    StreamBuffer body_buf(
        reinterpret_cast<const uint8_t*>(json_out.data()),
        reinterpret_cast<const uint8_t*>(json_out.data() + json_out.size()));
    conn->send_response(http_code, {{"Content-Type", "application/json"}}, body_buf);
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CliHttpServerActor::CliHttpServerActor(ActorContext* ctx, ActorSystem& system,
                                       const CliHttpServerConfig& config)
    : DaemonActor(ctx, system), system_(system), config_(config),
      gateway_(std::make_unique<net::HTTPGateway>()) {}

CliHttpServerActor::~CliHttpServerActor() = default;

// ---------------------------------------------------------------------------
// DaemonActor interface
// ---------------------------------------------------------------------------

void CliHttpServerActor::on_daemon_start() {
    build_command_tree();

    if (!gateway_->listen(config_.http_port, config_.http_bind_address)) {
        std::fprintf(stderr, "CliHttpServerActor: failed to listen on %s:%u\n",
                     config_.http_bind_address.c_str(),
                     static_cast<unsigned>(config_.http_port));
        listen_ok_ = false;
        return;
    }
    listen_ok_ = true;

    gateway_->set_request_handler(
        [this](net::HTTPConnection* conn, net::HttpRequest&& req) {
            on_http_request(conn, std::move(req));
        });

    gateway_->set_max_connections(config_.max_connections);
}

bool CliHttpServerActor::run_once() {
    if (!listen_ok_ || !running_)
        return false;

    gateway_->run_once();
    return running_;
}

void CliHttpServerActor::on_daemon_stop() {
    running_ = false;
    if (gateway_) {
        gateway_->stop();
    }
    command_tree_.reset();
}

// ---------------------------------------------------------------------------
// Command tree
// ---------------------------------------------------------------------------

void CliHttpServerActor::build_command_tree() {
    auto root = std::make_unique<CommandNode>("/", "CLI root");
    build_command_tree_from_registry(*root);
    command_tree_ = std::move(root);
}

// ---------------------------------------------------------------------------
// HTTP request handler
// ---------------------------------------------------------------------------

void CliHttpServerActor::on_http_request(net::HTTPConnection* conn,
                                         net::HttpRequest&& req) {
    // Only handle POST /cli
    if (req.method != net::HttpMethod::POST || req.path != "/cli") {
        std::string msg = "Not Found";
        StreamBuffer body_buf(
            reinterpret_cast<const uint8_t*>(msg.data()),
            reinterpret_cast<const uint8_t*>(msg.data() + msg.size()));
        conn->send_response(net::HttpStatusCode::NotFound,
                            {{"Content-Type", "text/plain"}}, body_buf);
        return;
    }

    // Extract body text
    std::string body_str(reinterpret_cast<const char*>(req.body.data()),
                         req.body.size());

    // Parse JSON body → CliCommand
    hpactor::cli::CliCommand cmd;
    if (!parse_cli_command_json(body_str, cmd)) {
        hpactor::cli::CliResponse err_resp;
        err_resp.set_content_type("text/plain");
        err_resp.set_payload("Invalid JSON in request body");
        err_resp.set_is_error(true);
        err_resp.set_error_code(400);
        err_resp.set_is_structured(false);
        send_json_response(conn, net::HttpStatusCode::BadRequest, err_resp);
        return;
    }

    // Reconstruct command line from CliCommand fields.
    // Path uses "/" as separator: "actor/42/show" → tokens "actor 42 show".
    std::string cmd_line = "/";
    if (!cmd.path().empty()) {
        for (char c : cmd.path()) {
            if (c == '/')
                cmd_line += ' ';
            else
                cmd_line += c;
        }
    }

    // Params: map of captured <param> values and --flags.
    for (const auto& [key, value] : cmd.params()) {
        if (value == "true") {
            cmd_line += " --" + key;
        } else {
            cmd_line += " --" + key + " " + value;
        }
    }

    // Format: explicit --format flag.
    std::string format = cmd.format();
    if (!format.empty()) {
        cmd_line += " --format " + format;
    }

    // Positional args.
    for (const auto& arg : cmd.args()) {
        cmd_line += " " + arg;
    }

    // Determine content_type for the CliResponse payload.
    std::string content_type = "text/plain";
    if (format == "json") {
        content_type = "application/json";
    }

    // Execute the command via CliSession.
    std::string output;
    {
        auto session = std::make_unique<CliSession>(
            &system_, command_tree_.get(),
            OutputFormatter::create(config_.default_format),
            [&output](const std::string& text) { output = text; },
            config_.page_size);
        session->set_system_host(this);
        session->set_lifecycle_host(this);

        session->process_line(cmd_line);
    }

    // Strip trailing newline appended by process_line.
    if (!output.empty() && output.back() == '\n') {
        output.pop_back();
    }

    // Build JSON CliResponse.
    hpactor::cli::CliResponse resp;
    resp.set_content_type(content_type);
    resp.set_payload(output);
    resp.set_is_error(false);
    resp.set_error_code(0);
    resp.set_is_structured(false);

    send_json_response(conn, net::HttpStatusCode::OK, resp);
}

// ---------------------------------------------------------------------------
// ISystemCliHost interface
// ---------------------------------------------------------------------------

void CliHttpServerActor::render_system_stats(OutputFormatter& output) {
    output.header("System Statistics");
    std::map<std::string, std::string> kv;
    kv["Total actors"] = std::to_string(system_.actor_count());
    if (auto* sched = system_.scheduler()) {
        kv["Scheduler threads"] = std::to_string(sched->worker_count());
    }
    output.key_value(kv);
}

void CliHttpServerActor::render_memory_stats(OutputFormatter& output) {
    output.header("Memory Regions");
    auto& reg = mem::MemoryRegionRegistry::instance();
    std::vector<std::string> cols = {"Region",     "Active", "Limit",
                                     "Pressure",   "Allocs", "Frees",
                                     "Corruptions"};
    std::vector<std::vector<std::string>> rows;
    static constexpr mem::RegionType kRegions[] = {
        mem::RegionType::kActor,     mem::RegionType::kMessage,
        mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
        mem::RegionType::kInternal,  mem::RegionType::kHibernate};
    for (auto region : kRegions) {
        auto snap = reg.snapshot(region);
        rows.push_back({
            mem::to_string(region),
            std::to_string(snap.active_bytes),
            std::to_string(snap.limit.hard_limit_bytes),
            mem::to_string(snap.pressure),
            std::to_string(snap.alloc_count),
            std::to_string(snap.free_count),
            std::to_string(snap.corruption_events),
        });
    }
    output.table(cols, rows);
}

void CliHttpServerActor::render_fault_status(OutputFormatter& output) {
    output.header("Fault Injection Status");
    auto& fc = system_.fault_controller();
    if (!fc.is_enabled()) {
        output.raw("Fault injection is disabled.\n");
        return;
    }
    std::map<std::string, std::string> kv;
    kv["Enabled"] = "yes";
    kv["Seed"] = std::to_string(fc.replay_seed());
    kv["Hooks triggered"] = std::to_string(fc.faults_fired());
    output.key_value(kv);
}

void CliHttpServerActor::render_dlq_list(OutputFormatter& output,
                                         std::string_view filter) {
    output.header("Dead Letter Queue");
    auto* dlq = system_.dead_letter_queue();
    if (!dlq) {
        output.raw("DLQ is not configured.\n");
        return;
    }
    auto records = dlq->snapshot_records();
    if (records.empty()) {
        output.raw("DLQ is empty.\n");
        return;
    }
    std::vector<std::string> cols = {"#", "Actor", "Reason", "Source", "Age"};
    std::vector<std::vector<std::string>> rows;
    for (size_t i = 0; i < records.size(); ++i) {
        auto& r = records[i];
        if (!filter.empty()) {
            std::string aid = std::to_string(r.target.id.value());
            if (aid.find(filter) == std::string::npos)
                continue;
        }
        rows.push_back({
            std::to_string(i),
            std::to_string(r.target.id.value()),
            mailbox::to_string(r.reason),
            mailbox::to_string(r.source),
            std::to_string(r.timestamp_ns / 1'000'000) + "ms",
        });
    }
    output.table(cols, rows);
}

result<void> CliHttpServerActor::dlq_replay(uint32_t index, ActorId target) {
    auto* dlq = system_.dead_letter_queue();
    if (!dlq)
        return result<void>::make(
            error(errors::actor_not_found, "DLQ not configured"));

    mailbox::DeadLetterRecord record;
    if (!dlq->try_pop_at(index, record))
        return result<void>::make(
            error(errors::invalid_argument, "DLQ index out of range"));

    TypedMessage msg(record.type_tag, std::move(record.payload_sample));
    msg.set_sender_address(address());
    auto enqueue_result = system_.try_deliver_local(target, std::move(msg));
    if (!enqueue_result.accepted())
        return result<void>::make(
            error(errors::mailbox_full, "replay delivery failed"));

    return result<void>::make();
}

// ---------------------------------------------------------------------------
// ILifecycleCliHost interface
// ---------------------------------------------------------------------------

result<void> CliHttpServerActor::drain() {
    return system_.shutdown();
}

result<void> CliHttpServerActor::shutdown() {
    return system_.shutdown();
}

// ---------------------------------------------------------------------------
// ICliCommandHost interface
// ---------------------------------------------------------------------------

std::optional<InspectStateReply>
CliHttpServerActor::inspect(ActorId target, const InspectStateRequest& req,
                            std::chrono::milliseconds timeout) {
    TypedMessage msg(TypeTag::InspectStateRequestTag, req);
    msg.set_sender_address(address());

    auto enq = system_.try_deliver_local(target, std::move(msg));
    if (!enq.accepted())
        return std::nullopt;

    // Poll mailbox for reply with timeout
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage m;
        if (mailbox()->try_pop(m)) {
            if (m.type_id() == TypeTag::InspectStateResponseTag) {
                auto reply = m.as<InspectStateReply>();
                if (reply)
                    return *reply;
                return std::nullopt;
            }
            // Drop unrelated messages (e.g., scheduled timers)
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

std::optional<KillReply>
CliHttpServerActor::kill(ActorId target, const KillRequest& req,
                         std::chrono::milliseconds timeout) {
    TypedMessage msg(TypeTag::KillRequestTag, req);
    msg.set_sender_address(address());

    auto enq = system_.try_deliver_local(target, std::move(msg));
    if (!enq.accepted())
        return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage m;
        if (mailbox()->try_pop(m)) {
            if (m.type_id() == TypeTag::KillResponseTag) {
                auto reply = m.as<KillReply>();
                if (reply)
                    return *reply;
                return std::nullopt;
            }
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

std::optional<QuarantineReply>
CliHttpServerActor::quarantine(ActorId target, const QuarantineRequest& req,
                               std::chrono::milliseconds timeout) {
    TypedMessage msg(TypeTag::QuarantineRequestTag, req);
    msg.set_sender_address(address());

    auto enq = system_.try_deliver_local(target, std::move(msg));
    if (!enq.accepted())
        return std::nullopt;

    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        TypedMessage m;
        if (mailbox()->try_pop(m)) {
            if (m.type_id() == TypeTag::QuarantineResponseTag) {
                auto reply = m.as<QuarantineReply>();
                if (reply)
                    return *reply;
                return std::nullopt;
            }
        }
        std::this_thread::yield();
    }
    return std::nullopt;
}

std::vector<ActorMeta> CliHttpServerActor::enumerate(std::string_view filter) {
    std::vector<ActorMeta> result;
    system_.for_each_actor([&](ActorId /*id*/, AbstractActor& actor) {
        auto meta = actor.to_metadata();
        if (!filter.empty()) {
            // Filter by type name substring match
            if (meta.actor_type.find(filter) == std::string::npos)
                return;
        }
        result.push_back(std::move(meta));
    });
    return result;
}

} // namespace cli
} // namespace hpactor
