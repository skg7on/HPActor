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
#include <hpactor/cli/cli_http_server_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <string>

namespace hpactor {
namespace cli {
namespace handlers {

using adt::JsonBuilder;

// ====================================================================
// Helper: build JSON object for a single DLQ record
// ====================================================================

// ====================================================================
// Task 12: handle_list_dlq
// ====================================================================

void handle_list_dlq(CliHttpServerActor* actor, net::HTTPConnection* conn,
                     net::HttpRequest&& req) {
    uint32_t offset = parse_offset(req);
    uint32_t limit = parse_limit(req);

    auto jb = JsonBuilder::root_object();

    auto* dlq = actor->system().dead_letter_queue();
    if (!dlq) {
        jb.array("data");
        jb.end_array();
        add_pagination(jb, offset, limit, static_cast<uint32_t>(0));
        send_json_ok(conn, jb.end_object().build());
        return;
    }

    auto records = dlq->snapshot_records();

    // Filter by actor_id if provided
    auto actor_id_str = parse_query_string(req, "actor_id");
    std::vector<size_t> filtered_indices;
    if (actor_id_str && !actor_id_str->empty()) {
        char* end = nullptr;
        uint64_t filter_id = std::strtoull(actor_id_str->c_str(), &end, 10);
        for (size_t i = 0; i < records.size(); ++i) {
            if (records[i].target.id.value() == filter_id) {
                filtered_indices.push_back(i);
            }
        }
    } else {
        for (size_t i = 0; i < records.size(); ++i) {
            filtered_indices.push_back(i);
        }
    }

    uint32_t total = static_cast<uint32_t>(filtered_indices.size());
    uint32_t start = (offset > total) ? total : offset;
    uint32_t end = (start + limit > total) ? total : (start + limit);

    jb.array("data");
    for (uint32_t i = start; i < end; ++i) {
        size_t idx = filtered_indices[i];
        const auto& record = records[idx];

        jb.object()
            .field("index", static_cast<uint64_t>(idx))
            .field("target_actor_id",
                   static_cast<uint64_t>(record.target.id.value()))
            .field("reason", std::string(mailbox::to_string(record.reason)))
            .field("source", std::string(mailbox::to_string(record.source)))
            .field("type_tag", static_cast<uint32_t>(record.type_tag))
            .field("timestamp_ms",
                   static_cast<uint64_t>(record.timestamp_ns / 1'000'000ULL))
            .field("payload_size_bytes", static_cast<uint64_t>(record.payload_size))
            .end_object();
    }
    jb.end_array();

    add_pagination(jb, offset, limit, total);

    send_json_ok(conn, jb.end_object().build());
}

// ====================================================================
// Task 12: handle_get_dlq_record
// ====================================================================

void handle_get_dlq_record(CliHttpServerActor* actor, net::HTTPConnection* conn,
                           net::HttpRequest&& req) {
    auto index_val = parse_path_uint64(req.path_params, "index");
    if (!index_val) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "index must be a non-negative integer");
        return;
    }

    auto* dlq = actor->system().dead_letter_queue();
    if (!dlq) {
        send_error(conn, net::HttpStatusCode::NotFound, "DLQ_NOT_CONFIGURED",
                   "Dead letter queue is not configured");
        return;
    }

    auto records = dlq->snapshot_records();
    if (*index_val >= records.size()) {
        send_error(conn, net::HttpStatusCode::NotFound, "DLQ_INDEX_OUT_OF_RANGE",
                   "DLQ index " + std::to_string(*index_val) +
                       " is out of range (max " +
                       std::to_string(records.size() - 1) + ")");
        return;
    }

    const auto& record = records[*index_val];

    std::string json =
        JsonBuilder::root_object()
            .object("data")
            .field("index", static_cast<uint64_t>(*index_val))
            .field("target_actor_id",
                   static_cast<uint64_t>(record.target.id.value()))
            .field("reason", std::string(mailbox::to_string(record.reason)))
            .field("source", std::string(mailbox::to_string(record.source)))
            .field("type_tag", static_cast<uint32_t>(record.type_tag))
            .field("timestamp_ms",
                   static_cast<uint64_t>(record.timestamp_ns / 1'000'000ULL))
            .field("payload_size_bytes", static_cast<uint64_t>(record.payload_size))
            .end_object()
            .end_object()
            .build();

    send_json_ok(conn, json);
}

// ====================================================================
// Task 12: handle_replay_dlq
// ====================================================================

void handle_replay_dlq(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req) {
    if (!validate_json_content_type(conn, req))
        return;

    auto index_val = parse_path_uint64(req.path_params, "index");
    if (!index_val) {
        send_error(conn, net::HttpStatusCode::BadRequest, "INVALID_FIELD",
                   "index must be a non-negative integer");
        return;
    }

    // Parse optional target_actor_id from JSON body
    ActorId target_id{0};
    std::string body_str(reinterpret_cast<const char*>(req.body.data()),
                         req.body.size());

    // Simple string search for "target_actor_id" in the JSON body
    std::string search_key = "\"target_actor_id\"";
    size_t pos = body_str.find(search_key);
    if (pos != std::string::npos) {
        pos += search_key.size();
        pos = body_str.find(':', pos);
        if (pos != std::string::npos) {
            ++pos;
            // Skip whitespace
            while (pos < body_str.size() &&
                   (body_str[pos] == ' ' || body_str[pos] == '\t')) {
                ++pos;
            }
            // Parse number
            if (pos < body_str.size() && body_str[pos] >= '0' &&
                body_str[pos] <= '9') {
                uint64_t val = 0;
                while (pos < body_str.size() && body_str[pos] >= '0' &&
                       body_str[pos] <= '9') {
                    val = val * 10 + static_cast<uint64_t>(body_str[pos] - '0');
                    ++pos;
                }
                target_id = ActorId{val};
            }
        }
    }

    // If no target specified in body, use the original target from the record.
    // Note: target_actor_id of 0 is a valid actor — no sentinel behavior.
    if (target_id.value() == 0 &&
        body_str.find("\"target_actor_id\"") == std::string::npos) {
        auto* dlq = actor->system().dead_letter_queue();
        if (dlq) {
            auto records = dlq->snapshot_records();
            if (*index_val < records.size()) {
                target_id = records[*index_val].target.id;
            }
        }
    }

    auto result = actor->dlq_replay(static_cast<uint32_t>(*index_val), target_id);
    if (result.ok()) {
        send_success(conn);
    } else {
        send_error(conn, net::HttpStatusCode::Conflict, "REPLAY_DELIVERY_FAILED",
                   "Failed to replay DLQ record " + std::to_string(*index_val));
    }
}

// ====================================================================
// Task 12: handle_export_dlq
// ====================================================================

void handle_export_dlq(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req) {
    (void)req;

    auto jb = JsonBuilder::root_object();
    jb.array("data");

    auto* dlq = actor->system().dead_letter_queue();
    if (dlq) {
        auto records = dlq->snapshot_records();
        for (size_t i = 0; i < records.size(); ++i) {
            const auto& record = records[i];
            jb.object()
                .field("index", static_cast<uint64_t>(i))
                .field("target_actor_id",
                       static_cast<uint64_t>(record.target.id.value()))
                .field("reason", std::string(mailbox::to_string(record.reason)))
                .field("source", std::string(mailbox::to_string(record.source)))
                .field("type_tag", static_cast<uint32_t>(record.type_tag))
                .field("timestamp_ms",
                       static_cast<uint64_t>(record.timestamp_ns / 1'000'000ULL))
                .field("payload_size_bytes",
                       static_cast<uint64_t>(record.payload_size))
                .end_object();
        }
    }

    jb.end_array();
    send_json_ok(conn, jb.end_object().build());
}

} // namespace handlers
} // namespace cli
} // namespace hpactor
