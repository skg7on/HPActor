// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "cli_http_handler_helpers.hpp"

#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli/cli_http_server_actor.hpp>
#include <hpactor/cli/http_handler.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <string>

namespace hpactor::cli::handlers {
using adt::JsonBuilder;

class ListDlqHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/dlq";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        uint32_t offset = parse_offset(req), limit = parse_limit(req);
        auto jb = JsonBuilder::root_object();
        auto* dlq = actor.system().dead_letter_queue();
        if (!dlq) {
            jb.array("data");
            jb.end_array();
            add_pagination(jb, offset, limit, 0);
            send_json_ok(&conn, jb.end_object().build());
            return;
        }
        auto records = dlq->snapshot_records();
        auto aid = parse_query_string(req, "actor_id");
        std::vector<size_t> fi;
        if (aid && !aid->empty()) {
            uint64_t fid = std::strtoull(aid->c_str(), nullptr, 10);
            for (size_t i = 0; i < records.size(); ++i)
                if (records[i].target.id.value() == fid)
                    fi.push_back(i);
        } else
            for (size_t i = 0; i < records.size(); ++i)
                fi.push_back(i);
        uint32_t total = static_cast<uint32_t>(fi.size());
        uint32_t start = (offset > total) ? total : offset;
        uint32_t end = (start + limit > total) ? total : (start + limit);
        jb.array("data");
        for (uint32_t i = start; i < end; ++i) {
            const auto& r = records[fi[i]];
            jb.object()
                .field("index", uint64_t(fi[i]))
                .field("target_actor_id", uint64_t(r.target.id.value()))
                .field("reason", std::string(mailbox::to_string(r.reason)))
                .field("source", std::string(mailbox::to_string(r.source)))
                .field("type_tag", uint32_t(r.type_tag))
                .field("timestamp_ms", uint64_t(r.timestamp_ns / 1'000'000ULL))
                .field("payload_size_bytes", uint64_t(r.payload_size))
                .end_object();
        }
        jb.end_array();
        add_pagination(jb, offset, limit, total);
        send_json_ok(&conn, jb.end_object().build());
    }
};

class GetDlqRecordHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/dlq/:index";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto iv = parse_path_uint64(req.path_params, "index");
        if (!iv) {
            send_error(&conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                       "index must be a non-negative integer");
            return;
        }
        auto* dlq = actor.system().dead_letter_queue();
        if (!dlq) {
            send_error(&conn, net::HttpStatusCode::NotFound, "DLQ_NOT_CONFIGURED",
                       "Dead letter queue is not configured");
            return;
        }
        auto records = dlq->snapshot_records();
        if (*iv >= records.size()) {
            send_error(&conn, net::HttpStatusCode::NotFound,
                       "DLQ_INDEX_OUT_OF_RANGE",
                       "DLQ index " + std::to_string(*iv) + " is out of range");
            return;
        }
        const auto& r = records[*iv];
        send_json_ok(
            &conn,
            JsonBuilder::root_object()
                .object("data")
                .field("index", uint64_t(*iv))
                .field("target_actor_id", uint64_t(r.target.id.value()))
                .field("reason", std::string(mailbox::to_string(r.reason)))
                .field("source", std::string(mailbox::to_string(r.source)))
                .field("type_tag", uint32_t(r.type_tag))
                .field("timestamp_ms", uint64_t(r.timestamp_ns / 1'000'000ULL))
                .field("payload_size_bytes", uint64_t(r.payload_size))
                .end_object()
                .end_object()
                .build());
    }
};

class ReplayDlqHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::POST;
    static constexpr std::string_view kPath = "/api/v1/dlq/:index/replay";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!validate_json_content_type(&conn, req))
            return;
        auto iv = parse_path_uint64(req.path_params, "index");
        if (!iv) {
            send_error(&conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                       "index must be a non-negative integer");
            return;
        }
        ActorId tid{0};
        std::string bs(reinterpret_cast<const char*>(req.body.data()),
                       req.body.size());
        std::string sk = "\"target_actor_id\"";
        size_t p = bs.find(sk);
        if (p != std::string::npos) {
            p += sk.size();
            p = bs.find(':', p);
            if (p != std::string::npos) {
                ++p;
                while (p < bs.size() && (bs[p] == ' ' || bs[p] == '\t'))
                    ++p;
                if (p < bs.size() && bs[p] >= '0' && bs[p] <= '9') {
                    uint64_t v = 0;
                    while (p < bs.size() && bs[p] >= '0' && bs[p] <= '9') {
                        v = v * 10 + uint64_t(bs[p] - '0');
                        ++p;
                    }
                    tid = ActorId{v};
                }
            }
        }
        if (tid.value() == 0 && bs.find("\"target_actor_id\"") == std::string::npos) {
            if (auto* d = actor.system().dead_letter_queue()) {
                auto rs = d->snapshot_records();
                if (*iv < rs.size())
                    tid = rs[*iv].target.id;
            }
        }
        auto rr = actor.dlq_replay(uint32_t(*iv), tid);
        if (rr.ok())
            send_success(&conn);
        else
            send_error(&conn, net::HttpStatusCode::Conflict,
                       "REPLAY_DELIVERY_FAILED",
                       "Failed to replay DLQ record " + std::to_string(*iv));
    }
};

class ExportDlqHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/dlq/export";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&&) override {
        auto jb = JsonBuilder::root_object();
        jb.array("data");
        if (auto* dlq = actor.system().dead_letter_queue()) {
            for (size_t i = 0; auto& r : dlq->snapshot_records()) {
                jb.object()
                    .field("index", uint64_t(i))
                    .field("target_actor_id", uint64_t(r.target.id.value()))
                    .field("reason", std::string(mailbox::to_string(r.reason)))
                    .field("source", std::string(mailbox::to_string(r.source)))
                    .field("type_tag", uint32_t(r.type_tag))
                    .field("timestamp_ms", uint64_t(r.timestamp_ns / 1'000'000ULL))
                    .field("payload_size_bytes", uint64_t(r.payload_size))
                    .end_object();
                ++i;
            }
        }
        jb.end_array();
        send_json_ok(&conn, jb.end_object().build());
    }
};

void register_dlq_handlers() {
    auto& r = HttpHandlerRegistry::instance();
    r.add(ListDlqHandler::kMethod, std::string(ListDlqHandler::kPath),
          std::make_unique<ListDlqHandler>());
    r.add(GetDlqRecordHandler::kMethod, std::string(GetDlqRecordHandler::kPath),
          std::make_unique<GetDlqRecordHandler>());
    r.add(ReplayDlqHandler::kMethod, std::string(ReplayDlqHandler::kPath),
          std::make_unique<ReplayDlqHandler>());
    r.add(ExportDlqHandler::kMethod, std::string(ExportDlqHandler::kPath),
          std::make_unique<ExportDlqHandler>());
}

} // namespace hpactor::cli::handlers
