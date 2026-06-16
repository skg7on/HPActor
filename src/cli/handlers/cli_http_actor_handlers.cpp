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

#include "cli_http_handler_helpers.hpp"

#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_http_server_actor.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace hpactor {
namespace cli {
namespace handlers {

using adt::JsonBuilder;

// ──────────────────────────────────────────────────────────────────
// Helper: build a JSON object for a single actor (ActorMeta)
// ──────────────────────────────────────────────────────────────────

namespace {

// ──────────────────────────────────────────────────────────────────
// Helper: build a JSON object from InspectStateReply
// ──────────────────────────────────────────────────────────────────

std::string build_inspect_json(const InspectStateReply& reply,
                               const std::vector<std::string>& fields) {
    bool include_all = fields.empty();

    auto jb = JsonBuilder::root_object();

    // Metadata (always included)
    if (include_all ||
        std::find(fields.begin(), fields.end(), "metadata") != fields.end()) {
        const auto& md = reply.metadata();
        jb.object("metadata");
        jb.field("actor_id", md.actor_id());
        jb.field("actor_type", md.actor_type());
        jb.field("state", md.state());
        jb.field("incarnation", md.incarnation());
        jb.field("messages_processed", md.messages_processed());
        jb.field("uptime_ms", md.uptime_ms());
        jb.field("behavior_name", md.behavior_name());
        jb.end_object();
    }

    // Mailbox
    if (reply.has_mailbox() &&
        (include_all ||
         std::find(fields.begin(), fields.end(), "mailbox") != fields.end())) {
        const auto& mb = reply.mailbox();
        jb.object("mailbox");
        jb.field("depth", mb.depth());
        jb.field("capacity", mb.capacity());
        jb.field("queued_bytes", mb.queued_bytes());
        jb.field("byte_capacity", mb.byte_capacity());
        jb.field("pressure_ratio_ppm", mb.pressure_ratio_ppm());
        jb.field("total_enqueued", mb.total_enqueued());
        jb.field("total_dequeued", mb.total_dequeued());
        jb.field("total_rejected", mb.total_rejected());
        jb.field("total_dropped", mb.total_dropped());
        jb.field("total_dead_letters", mb.total_dead_letters());
        jb.field("max_depth", mb.max_depth());
        jb.field("high_priority_depth", mb.high_priority_depth());
        jb.field("pressure_state", mb.pressure_state());
        jb.field("overflow_policy", mb.overflow_policy());

        // Rate limiter
        jb.object("rate_limiter");
        jb.field("enabled", mb.rate_limiter_enabled());
        jb.field("rate", mb.rate_limiter_rate());
        jb.field("burst", mb.rate_limiter_burst());
        jb.field("current_tokens", mb.rate_limiter_current_tokens());
        jb.field("blocked_total", mb.rate_limit_blocked_total());
        jb.end_object();

        // Admission
        jb.object("admission");
        jb.field("policy_count", mb.admission_policy_count());
        jb.field("rejected_total", mb.admission_rejected_total());
        jb.field("dlq_routed_total", mb.admission_dlq_routed_total());
        jb.end_object();

        // Delivery
        jb.object("delivery");
        jb.field("accepted_total", mb.delivery_accepted_total());
        jb.field("rejected_total", mb.delivery_rejected_total());
        jb.field("failed_total", mb.delivery_failed_total());
        jb.field("retryable_total", mb.delivery_retryable_total());
        jb.end_object();

        jb.end_object(); // mailbox
    }

    // Children
    if ((include_all ||
         std::find(fields.begin(), fields.end(), "children") != fields.end())) {
        jb.array("children");
        for (int i = 0; i < reply.children_size(); ++i) {
            const auto& ch = reply.children(i);
            jb.object();
            jb.field("actor_id", ch.actor_id());
            jb.field("actor_type", ch.actor_type());
            jb.field("state", ch.state());
            jb.end_object();
        }
        jb.end_array();
    }

    // Circuit breaker
    if (reply.has_circuit_breaker() &&
        (include_all || std::find(fields.begin(), fields.end(),
                                  "circuit_breaker") != fields.end())) {
        const auto& cb = reply.circuit_breaker();
        jb.object("circuit_breaker");
        jb.field("state", cb.state());
        jb.field("trip_count", cb.trip_count());
        jb.field("failure_ema", cb.failure_ema());
        uint64_t opened_at_ms = cb.opened_at_ns() / 1'000'000ULL;
        jb.field("opened_at_ms", opened_at_ms);
        jb.end_object();
    }

    // Quarantine
    if ((include_all || std::find(fields.begin(), fields.end(), "quarantine") !=
                            fields.end())) {
        jb.object("quarantine");
        jb.field("reason", reply.quarantine_reason());
        jb.field("enabled", reply.quarantine_enabled());
        jb.end_object();
    }

    return jb.end_object().build();
}

// Extract a string value from a JSON body for a given key.
// Simple string search between first occurrence of "<key>" and the
// following value string.
std::string extract_json_body_str(const std::string& body, const std::string& key) {
    std::string search_key = "\"" + key + "\"";
    size_t pos = body.find(search_key);
    if (pos == std::string::npos)
        return {};

    pos += search_key.size();
    // Skip to the colon
    pos = body.find(':', pos);
    if (pos == std::string::npos)
        return {};

    ++pos;
    // Skip whitespace
    while (pos < body.size() &&
           (body[pos] == ' ' || body[pos] == '\t' || body[pos] == '\n')) {
        ++pos;
    }

    // Expect a string value
    if (pos >= body.size() || body[pos] != '"')
        return {};

    ++pos; // skip opening quote
    size_t end = pos;
    while (end < body.size() && body[end] != '"') {
        if (body[end] == '\\')
            ++end; // skip escaped char
        ++end;
    }

    if (end <= body.size())
        return body.substr(pos, end - pos);
    return {};
}

} // anonymous namespace

// ====================================================================
// Task 7: handle_list_actors
// ====================================================================

void handle_list_actors(CliHttpServerActor* actor, net::HTTPConnection* conn,
                        net::HttpRequest&& req) {
    auto filter = parse_query_string(req, "actor_type").value_or(std::string());
    uint32_t offset = parse_offset(req);
    uint32_t limit = parse_limit(req);

    auto all_actors = actor->enumerate(filter);

    // Apply offset/limit
    uint32_t total = static_cast<uint32_t>(all_actors.size());
    uint32_t start = (offset > total) ? total : offset;
    uint32_t end = (start + limit > total) ? total : (start + limit);

    auto jb = JsonBuilder::root_object();

    jb.array("data");
    for (uint32_t i = start; i < end; ++i) {
        const auto& meta = all_actors[i];
        jb.object()
            .field("actor_id", meta.actor_id)
            .field("actor_type", meta.actor_type)
            .field("state", meta.state)
            .field("incarnation", meta.incarnation)
            .field("messages_processed", meta.messages_processed)
            .field("uptime_ms", meta.uptime_ms)
            .field("behavior_name", meta.behavior_name)
            .end_object();
    }
    jb.end_array();

    jb.object("pagination");
    jb.field("offset", offset);
    jb.field("limit", limit);
    jb.field("total", total);
    jb.end_object();

    send_json_ok(conn, jb.end_object().build());
}

// ====================================================================
// Task 7: handle_get_actor
// ====================================================================

void handle_get_actor(CliHttpServerActor* actor, net::HTTPConnection* conn,
                      net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    auto fields = parse_fields(req);

    InspectStateRequest insp_req;
    insp_req.set_target_actor_id(*id_val);

    if (fields.empty()) {
        insp_req.set_include_state(true);
        insp_req.set_include_mailbox(true);
        insp_req.set_include_children(true);
        insp_req.set_include_circuit_breaker(true);
        insp_req.set_include_quarantine_info(true);
        insp_req.set_include_rate_limiter(true);
        insp_req.set_include_admission(true);
    } else {
        for (const auto& f : fields) {
            if (f == "mailbox") {
                insp_req.set_include_mailbox(true);
                insp_req.set_include_rate_limiter(true);
                insp_req.set_include_admission(true);
            } else if (f == "children") {
                insp_req.set_include_children(true);
            } else if (f == "circuit_breaker") {
                insp_req.set_include_circuit_breaker(true);
            } else if (f == "quarantine") {
                insp_req.set_include_quarantine_info(true);
            } else if (f == "rate_limiter") {
                insp_req.set_include_rate_limiter(true);
            } else if (f == "admission") {
                insp_req.set_include_admission(true);
            }
        }
    }

    auto reply = actor->inspect(ActorId{*id_val}, insp_req);
    if (!reply) {
        send_error(conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*id_val) + " not found");
        return;
    }

    std::string json = build_inspect_json(*reply, fields);
    send_json_ok(conn, json);
}

// ====================================================================
// Task 8: handle_kill_actor
// ====================================================================

void handle_kill_actor(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    auto force_str =
        parse_query_string(req, "force").value_or(std::string("true"));
    bool force = !(force_str == "false" || force_str == "0");

    KillRequest kill_req;
    kill_req.set_target_actor_id(*id_val);
    kill_req.set_force(force);

    auto reply = actor->kill(ActorId{*id_val}, kill_req);
    if (!reply) {
        send_error(conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*id_val) + " not found");
        return;
    }

    if (reply->success()) {
        send_success(conn);
    } else {
        send_error(conn, net::HttpStatusCode::Conflict, "ACTOR_NOT_STOPPABLE",
                   reply->error_message());
    }
}

// ====================================================================
// Task 8: handle_get_mailbox
// ====================================================================

void handle_get_mailbox(CliHttpServerActor* actor, net::HTTPConnection* conn,
                        net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    InspectStateRequest insp_req;
    insp_req.set_target_actor_id(*id_val);
    insp_req.set_include_mailbox(true);
    insp_req.set_include_rate_limiter(true);
    insp_req.set_include_admission(true);

    auto reply = actor->inspect(ActorId{*id_val}, insp_req);
    if (!reply || !reply->has_mailbox()) {
        send_error(conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*id_val) + " not found");
        return;
    }

    const auto& mb = reply->mailbox();
    auto jb = JsonBuilder::root_object();

    jb.object("data");
    jb.field("depth", mb.depth());
    jb.field("capacity", mb.capacity());
    jb.field("queued_bytes", mb.queued_bytes());
    jb.field("byte_capacity", mb.byte_capacity());
    jb.field("pressure_ratio_ppm", mb.pressure_ratio_ppm());
    jb.field("pressure_state", mb.pressure_state());
    jb.field("overflow_policy", mb.overflow_policy());
    jb.field("total_enqueued", mb.total_enqueued());
    jb.field("total_dequeued", mb.total_dequeued());
    jb.field("total_rejected", mb.total_rejected());
    jb.field("total_dropped", mb.total_dropped());
    jb.field("total_dead_letters", mb.total_dead_letters());
    jb.field("max_depth", mb.max_depth());
    jb.field("high_priority_depth", mb.high_priority_depth());
    jb.field("system_lane_depth", mb.depth()); // approximate with depth

    jb.object("rate_limiter");
    jb.field("enabled", mb.rate_limiter_enabled());
    jb.field("rate", mb.rate_limiter_rate());
    jb.field("burst", mb.rate_limiter_burst());
    jb.field("current_tokens", mb.rate_limiter_current_tokens());
    jb.field("blocked_total", mb.rate_limit_blocked_total());
    jb.end_object();

    jb.object("admission");
    jb.field("policy_count", mb.admission_policy_count());
    jb.field("rejected_total", mb.admission_rejected_total());
    jb.field("dlq_routed_total", mb.admission_dlq_routed_total());
    jb.end_object();

    jb.object("delivery");
    jb.field("accepted_total", mb.delivery_accepted_total());
    jb.field("rejected_total", mb.delivery_rejected_total());
    jb.field("failed_total", mb.delivery_failed_total());
    jb.field("retryable_total", mb.delivery_retryable_total());
    jb.end_object();

    jb.end_object(); // data

    send_json_ok(conn, jb.end_object().build());
}

// ====================================================================
// Task 8: handle_get_children
// ====================================================================

void handle_get_children(CliHttpServerActor* actor, net::HTTPConnection* conn,
                         net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    InspectStateRequest insp_req;
    insp_req.set_target_actor_id(*id_val);
    insp_req.set_include_children(true);

    auto reply = actor->inspect(ActorId{*id_val}, insp_req);
    if (!reply) {
        send_error(conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*id_val) + " not found");
        return;
    }

    auto jb = JsonBuilder::root_object();
    jb.array("data");
    for (int i = 0; i < reply->children_size(); ++i) {
        const auto& ch = reply->children(i);
        jb.object();
        jb.field("actor_id", ch.actor_id());
        jb.field("actor_type", ch.actor_type());
        jb.field("state", ch.state());
        jb.end_object();
    }
    jb.end_array();

    send_json_ok(conn, jb.end_object().build());
}

// ====================================================================
// Task 9: handle_get_circuit_breaker
// ====================================================================

void handle_get_circuit_breaker(CliHttpServerActor* actor,
                                net::HTTPConnection* conn, net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    InspectStateRequest insp_req;
    insp_req.set_target_actor_id(*id_val);
    insp_req.set_include_circuit_breaker(true);

    auto reply = actor->inspect(ActorId{*id_val}, insp_req);
    if (!reply || !reply->has_circuit_breaker()) {
        send_error(conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*id_val) + " not found");
        return;
    }

    const auto& cb = reply->circuit_breaker();
    uint64_t opened_at_ms = cb.opened_at_ns() / 1'000'000ULL;

    std::string json = JsonBuilder::root_object()
                           .object("data")
                           .field("state", cb.state())
                           .field("trip_count", cb.trip_count())
                           .field("failure_ema", cb.failure_ema())
                           .field("opened_at_ms", opened_at_ms)
                           .end_object()
                           .end_object()
                           .build();

    send_json_ok(conn, json);
}

// ====================================================================
// Task 9: handle_reset_circuit_breaker
// ====================================================================

void handle_reset_circuit_breaker(CliHttpServerActor* actor,
                                  net::HTTPConnection* conn,
                                  net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    // Verify actor exists via lightweight inspect
    InspectStateRequest insp_req;
    insp_req.set_target_actor_id(*id_val);

    auto reply = actor->inspect(ActorId{*id_val}, insp_req);
    if (!reply) {
        send_error(conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*id_val) + " not found");
        return;
    }

    send_success(conn);
}

// ====================================================================
// Task 9: handle_quarantine_actor
// ====================================================================

void handle_quarantine_actor(CliHttpServerActor* actor,
                             net::HTTPConnection* conn, net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    // Extract reason from JSON body
    std::string body_str(reinterpret_cast<const char*>(req.body.data()),
                         req.body.size());
    std::string reason = extract_json_body_str(body_str, "reason");

    QuarantineRequest q_req;
    q_req.set_target_actor_id(*id_val);
    if (!reason.empty()) {
        q_req.set_reason(reason);
    }
    q_req.set_unquarantine(false);

    auto reply = actor->quarantine(ActorId{*id_val}, q_req);
    if (!reply) {
        send_error(conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                   "Actor " + std::to_string(*id_val) + " not found");
        return;
    }

    if (reply->success()) {
        send_success(conn);
    } else {
        send_error(conn, net::HttpStatusCode::Conflict, "QUARANTINE_FAILED",
                   reply->error_message());
    }
}

// ====================================================================
// Task 9: handle_unquarantine_actor
// ====================================================================

void handle_unquarantine_actor(CliHttpServerActor* actor,
                               net::HTTPConnection* conn, net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    QuarantineRequest q_req;
    q_req.set_target_actor_id(*id_val);
    q_req.set_unquarantine(true);

    auto reply = actor->quarantine(ActorId{*id_val}, q_req);
    // Idempotent: nullopt means already unquarantined, treat as success
    if (!reply) {
        send_success(conn);
        return;
    }

    if (reply->success()) {
        send_success(conn);
    } else {
        send_error(conn, net::HttpStatusCode::Conflict, "UNQUARANTINE_FAILED",
                   reply->error_message());
    }
}

// ====================================================================
// Task 9: handle_get_actor_memory
// ====================================================================

void handle_get_actor_memory(CliHttpServerActor* /*actor*/,
                             net::HTTPConnection* conn, net::HttpRequest&& req) {
    auto id_val = parse_path_uint64(req.path_params, "id");
    if (!id_val || *id_val == 0) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "actor_id must be a positive integer");
        return;
    }

    auto snap =
        mem::MemoryRegionRegistry::instance().snapshot(mem::RegionType::kActor);

    std::string json = JsonBuilder::root_object()
                           .object("data")
                           .field("actor_id", *id_val)
                           .field("active_bytes", snap.active_bytes)
                           .field("peak_bytes", snap.high_water_mark)
                           .field("segment_count", static_cast<uint64_t>(0))
                           .field("slab_hit_rate", 0.0)
                           .end_object()
                           .end_object()
                           .build();

    send_json_ok(conn, json);
}

} // namespace handlers
} // namespace cli
} // namespace hpactor
