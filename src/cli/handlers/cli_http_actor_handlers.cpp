// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "cli_http_handler_helpers.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli/cli_command_host.hpp>
#include <hpactor/cli/cli_http_server_actor.hpp>
#include <hpactor/cli/http_handler.hpp>
#include <hpactor/cli_messages.pb.h>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

#include <algorithm>
#include <cstdint>
#include <string>

namespace hpactor::cli::handlers {
using adt::JsonBuilder;
namespace {
std::string build_inspect_json(const InspectStateReply& reply,
                               const std::vector<std::string>& fields) {
    bool ia = fields.empty();
    auto jb = JsonBuilder::root_object();
    if (ia || std::find(fields.begin(), fields.end(), "metadata") != fields.end()) {
        const auto& md = reply.metadata();
        jb.object("metadata");
        jb.field("actor_id", md.actor_id())
            .field("actor_type", md.actor_type())
            .field("state", md.state())
            .field("incarnation", md.incarnation())
            .field("messages_processed", md.messages_processed())
            .field("uptime_ms", md.uptime_ms())
            .field("behavior_name", md.behavior_name());
        jb.end_object();
    }
    if (reply.has_mailbox() && (ia || std::find(fields.begin(), fields.end(),
                                                "mailbox") != fields.end())) {
        const auto& mb = reply.mailbox();
        jb.object("mailbox");
        jb.field("depth", mb.depth())
            .field("capacity", mb.capacity())
            .field("queued_bytes", mb.queued_bytes())
            .field("byte_capacity", mb.byte_capacity())
            .field("pressure_ratio_ppm", mb.pressure_ratio_ppm())
            .field("total_enqueued", mb.total_enqueued())
            .field("total_dequeued", mb.total_dequeued())
            .field("total_rejected", mb.total_rejected())
            .field("total_dropped", mb.total_dropped())
            .field("total_dead_letters", mb.total_dead_letters())
            .field("max_depth", mb.max_depth())
            .field("high_priority_depth", mb.high_priority_depth())
            .field("pressure_state", mb.pressure_state())
            .field("overflow_policy", mb.overflow_policy());
        jb.object("rate_limiter");
        jb.field("enabled", mb.rate_limiter_enabled())
            .field("rate", mb.rate_limiter_rate())
            .field("burst", mb.rate_limiter_burst())
            .field("current_tokens", mb.rate_limiter_current_tokens())
            .field("blocked_total", mb.rate_limit_blocked_total());
        jb.end_object();
        jb.object("admission");
        jb.field("policy_count", mb.admission_policy_count())
            .field("rejected_total", mb.admission_rejected_total())
            .field("dlq_routed_total", mb.admission_dlq_routed_total());
        jb.end_object();
        jb.object("delivery");
        jb.field("accepted_total", mb.delivery_accepted_total())
            .field("rejected_total", mb.delivery_rejected_total())
            .field("failed_total", mb.delivery_failed_total())
            .field("retryable_total", mb.delivery_retryable_total());
        jb.end_object();
        jb.end_object();
    }
    if (ia || std::find(fields.begin(), fields.end(), "children") != fields.end()) {
        jb.array("children");
        for (int i = 0; i < reply.children_size(); ++i) {
            const auto& ch = reply.children(i);
            jb.object();
            jb.field("actor_id", ch.actor_id())
                .field("actor_type", ch.actor_type())
                .field("state", ch.state());
            jb.end_object();
        }
        jb.end_array();
    }
    if (reply.has_circuit_breaker() &&
        (ia || std::find(fields.begin(), fields.end(), "circuit_breaker") !=
                   fields.end())) {
        const auto& cb = reply.circuit_breaker();
        jb.object("circuit_breaker");
        jb.field("state", cb.state())
            .field("trip_count", cb.trip_count())
            .field("failure_ema", cb.failure_ema());
        jb.field("opened_at_ms", cb.opened_at_ns() / 1'000'000ULL);
        jb.end_object();
    }
    if (ia ||
        std::find(fields.begin(), fields.end(), "quarantine") != fields.end()) {
        jb.object("quarantine");
        jb.field("reason", reply.quarantine_reason())
            .field("enabled", reply.quarantine_enabled());
        jb.end_object();
    }
    return jb.end_object().build();
}
std::string extract_json_body_str(const std::string& body, const std::string& key) {
    std::string sk = "\"" + key + "\"";
    size_t p = body.find(sk);
    if (p == std::string::npos)
        return {};
    p += sk.size();
    p = body.find(':', p);
    if (p == std::string::npos)
        return {};
    ++p;
    while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == '\n'))
        ++p;
    if (p >= body.size() || body[p] != '"')
        return {};
    ++p;
    size_t e = p;
    while (e < body.size() && body[e] != '"') {
        if (body[e] == '\\')
            ++e;
        ++e;
    }
    if (e <= body.size())
        return adt::json_unescape(body.substr(p, e - p));
    return {};
}
} // namespace

class ListActorsHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/actors";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto filter =
            parse_query_string(req, "actor_type").value_or(std::string());
        uint32_t offset = parse_offset(req), limit = parse_limit(req);
        auto aa = actor.enumerate(filter);
        uint32_t total = uint32_t(aa.size());
        uint32_t start = (offset > total) ? total : offset;
        uint32_t end = (start + limit > total) ? total : (start + limit);
        auto jb = JsonBuilder::root_object();
        jb.array("data");
        for (uint32_t i = start; i < end; ++i) {
            const auto& m = aa[i];
            jb.object()
                .field("actor_id", m.actor_id)
                .field("actor_type", m.actor_type)
                .field("state", m.state)
                .field("incarnation", m.incarnation)
                .field("messages_processed", m.messages_processed)
                .field("uptime_ms", m.uptime_ms)
                .field("behavior_name", m.behavior_name)
                .end_object();
        }
        jb.end_array();
        add_pagination(jb, offset, limit, total);
        send_json_ok(&conn, jb.end_object().build());
    }
};

class GetActorHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/actors/:id";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        auto fields = parse_fields(req);
        InspectStateRequest ir;
        ir.set_target_actor_id(*iv);
        if (fields.empty()) {
            ir.set_include_state(true);
            ir.set_include_mailbox(true);
            ir.set_include_children(true);
            ir.set_include_circuit_breaker(true);
            ir.set_include_quarantine_info(true);
            ir.set_include_rate_limiter(true);
            ir.set_include_admission(true);
        } else
            for (auto& f : fields) {
                if (f == "mailbox") {
                    ir.set_include_mailbox(true);
                    ir.set_include_rate_limiter(true);
                    ir.set_include_admission(true);
                } else if (f == "children")
                    ir.set_include_children(true);
                else if (f == "circuit_breaker")
                    ir.set_include_circuit_breaker(true);
                else if (f == "quarantine")
                    ir.set_include_quarantine_info(true);
                else if (f == "rate_limiter")
                    ir.set_include_rate_limiter(true);
                else if (f == "admission")
                    ir.set_include_admission(true);
            }
        auto r = actor.inspect(ActorId{*iv}, ir);
        if (!r) {
            send_error(&conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                       "Actor " + std::to_string(*iv) +
                           " not found or not responding");
            return;
        }
        send_json_ok(&conn, build_inspect_json(*r, fields));
    }
};

class KillActorHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::DELETE;
    static constexpr std::string_view kPath = "/api/v1/actors/:id";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        auto fs = parse_query_string(req, "force").value_or(std::string("true"));
        bool force = !(fs == "false" || fs == "0");
        KillRequest kr;
        kr.set_target_actor_id(*iv);
        kr.set_force(force);
        auto r = actor.kill(ActorId{*iv}, kr);
        if (!r) {
            send_error(&conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                       "Actor " + std::to_string(*iv) +
                           " not found or not responding");
            return;
        }
        if (r->success())
            send_success(&conn);
        else
            send_error(&conn, net::HttpStatusCode::Conflict,
                       "ACTOR_NOT_STOPPABLE", r->error_message());
    }
};

class GetMailboxHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/actors/:id/mailbox";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        InspectStateRequest ir;
        ir.set_target_actor_id(*iv);
        ir.set_include_mailbox(true);
        ir.set_include_rate_limiter(true);
        ir.set_include_admission(true);
        auto r = actor.inspect(ActorId{*iv}, ir);
        if (!r || !r->has_mailbox()) {
            send_error(&conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                       "Actor " + std::to_string(*iv) +
                           " not found or not responding");
            return;
        }
        const auto& mb = r->mailbox();
        auto jb = JsonBuilder::root_object();
        jb.object("data");
        jb.field("depth", mb.depth())
            .field("capacity", mb.capacity())
            .field("queued_bytes", mb.queued_bytes())
            .field("byte_capacity", mb.byte_capacity())
            .field("pressure_ratio_ppm", mb.pressure_ratio_ppm())
            .field("pressure_state", mb.pressure_state())
            .field("overflow_policy", mb.overflow_policy())
            .field("total_enqueued", mb.total_enqueued())
            .field("total_dequeued", mb.total_dequeued())
            .field("total_rejected", mb.total_rejected())
            .field("total_dropped", mb.total_dropped())
            .field("total_dead_letters", mb.total_dead_letters())
            .field("max_depth", mb.max_depth())
            .field("high_priority_depth", mb.high_priority_depth())
            .field("system_lane_depth", mb.depth());
        jb.object("rate_limiter");
        jb.field("enabled", mb.rate_limiter_enabled())
            .field("rate", mb.rate_limiter_rate())
            .field("burst", mb.rate_limiter_burst())
            .field("current_tokens", mb.rate_limiter_current_tokens())
            .field("blocked_total", mb.rate_limit_blocked_total());
        jb.end_object();
        jb.object("admission");
        jb.field("policy_count", mb.admission_policy_count())
            .field("rejected_total", mb.admission_rejected_total())
            .field("dlq_routed_total", mb.admission_dlq_routed_total());
        jb.end_object();
        jb.object("delivery");
        jb.field("accepted_total", mb.delivery_accepted_total())
            .field("rejected_total", mb.delivery_rejected_total())
            .field("failed_total", mb.delivery_failed_total())
            .field("retryable_total", mb.delivery_retryable_total());
        jb.end_object();
        jb.end_object();
        send_json_ok(&conn, jb.end_object().build());
    }
};

class GetChildrenHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/actors/:id/children";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        InspectStateRequest ir;
        ir.set_target_actor_id(*iv);
        ir.set_include_children(true);
        auto r = actor.inspect(ActorId{*iv}, ir);
        if (!r) {
            send_error(&conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                       "Actor " + std::to_string(*iv) +
                           " not found or not responding");
            return;
        }
        auto jb = JsonBuilder::root_object();
        jb.array("data");
        for (int i = 0; i < r->children_size(); ++i) {
            const auto& ch = r->children(i);
            jb.object();
            jb.field("actor_id", ch.actor_id())
                .field("actor_type", ch.actor_type())
                .field("state", ch.state());
            jb.end_object();
        }
        jb.end_array();
        send_json_ok(&conn, jb.end_object().build());
    }
};

class GetCircuitBreakerHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/actors/:id/circuit-breaker";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        InspectStateRequest ir;
        ir.set_target_actor_id(*iv);
        ir.set_include_circuit_breaker(true);
        auto r = actor.inspect(ActorId{*iv}, ir);
        if (!r || !r->has_circuit_breaker()) {
            send_error(&conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                       "Actor " + std::to_string(*iv) +
                           " not found or not responding");
            return;
        }
        const auto& cb = r->circuit_breaker();
        auto oms = cb.opened_at_ns() / 1'000'000ULL;
        send_json_ok(&conn, JsonBuilder::root_object()
                                .object("data")
                                .field("state", cb.state())
                                .field("trip_count", cb.trip_count())
                                .field("failure_ema", cb.failure_ema())
                                .field("opened_at_ms", oms)
                                .end_object()
                                .end_object()
                                .build());
    }
};

class ResetCircuitBreakerHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::POST;
    static constexpr std::string_view kPath =
        "/api/v1/actors/:id/circuit-breaker/reset";
    void handle(CliHttpServerActor&, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!validate_json_content_type(&conn, req))
            return;
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        send_error(&conn, net::HttpStatusCode::NotImplemented, "NOT_IMPLEMENTED",
                   "Circuit breaker reset not yet implemented");
    }
};

class QuarantineActorHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::POST;
    static constexpr std::string_view kPath = "/api/v1/actors/:id/quarantine";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!validate_json_content_type(&conn, req))
            return;
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        std::string bs(reinterpret_cast<const char*>(req.body.data()),
                       req.body.size());
        std::string reason = extract_json_body_str(bs, "reason");
        QuarantineRequest qr;
        qr.set_target_actor_id(*iv);
        if (!reason.empty())
            qr.set_reason(reason);
        qr.set_unquarantine(false);
        auto r = actor.quarantine(ActorId{*iv}, qr);
        if (!r) {
            send_error(&conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                       "Actor " + std::to_string(*iv) +
                           " not found or not responding");
            return;
        }
        if (r->success())
            send_success(&conn);
        else
            send_error(&conn, net::HttpStatusCode::Conflict,
                       "QUARANTINE_FAILED", r->error_message());
    }
};

class UnquarantineActorHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::DELETE;
    static constexpr std::string_view kPath = "/api/v1/actors/:id/quarantine";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        QuarantineRequest qr;
        qr.set_target_actor_id(*iv);
        qr.set_unquarantine(true);
        auto r = actor.quarantine(ActorId{*iv}, qr);
        if (!r) {
            send_error(&conn, net::HttpStatusCode::NotFound, "ACTOR_NOT_FOUND",
                       "Actor " + std::to_string(*iv) +
                           " not found or not responding");
            return;
        }
        if (r->success())
            send_success(&conn);
        else
            send_error(&conn, net::HttpStatusCode::Conflict,
                       "UNQUARANTINE_FAILED", r->error_message());
    }
};

class GetActorMemoryHandler final : public IHttpHandler {
  public:
    static constexpr auto kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/actors/:id/memory";
    void handle(CliHttpServerActor&, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_actor_id_or_error(req.path_params, &conn);
        if (!iv)
            return;
        send_error(
            &conn, net::HttpStatusCode::NotImplemented, "NOT_IMPLEMENTED",
            "Per-actor memory stats not yet available. Use GET /api/v1/system/memory?actor_id=N for system-wide memory.");
    }
};

void register_actor_handlers() {
    auto& r = HttpHandlerRegistry::instance();
    r.add(ListActorsHandler::kMethod, std::string(ListActorsHandler::kPath),
          std::make_unique<ListActorsHandler>());
    r.add(GetActorHandler::kMethod, std::string(GetActorHandler::kPath),
          std::make_unique<GetActorHandler>());
    r.add(KillActorHandler::kMethod, std::string(KillActorHandler::kPath),
          std::make_unique<KillActorHandler>());
    r.add(GetMailboxHandler::kMethod, std::string(GetMailboxHandler::kPath),
          std::make_unique<GetMailboxHandler>());
    r.add(GetChildrenHandler::kMethod, std::string(GetChildrenHandler::kPath),
          std::make_unique<GetChildrenHandler>());
    r.add(GetCircuitBreakerHandler::kMethod,
          std::string(GetCircuitBreakerHandler::kPath),
          std::make_unique<GetCircuitBreakerHandler>());
    r.add(ResetCircuitBreakerHandler::kMethod,
          std::string(ResetCircuitBreakerHandler::kPath),
          std::make_unique<ResetCircuitBreakerHandler>());
    r.add(QuarantineActorHandler::kMethod,
          std::string(QuarantineActorHandler::kPath),
          std::make_unique<QuarantineActorHandler>());
    r.add(UnquarantineActorHandler::kMethod,
          std::string(UnquarantineActorHandler::kPath),
          std::make_unique<UnquarantineActorHandler>());
    r.add(GetActorMemoryHandler::kMethod, std::string(GetActorMemoryHandler::kPath),
          std::make_unique<GetActorMemoryHandler>());
}

} // namespace hpactor::cli::handlers
