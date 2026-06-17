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
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

#include <string>

namespace hpactor {
namespace cli {
namespace handlers {

using adt::JsonBuilder;

// ====================================================================
// Task 10: handle_api_index
// ====================================================================

void handle_api_index(CliHttpServerActor* actor, net::HTTPConnection* conn,
                      net::HttpRequest&& req) {
    // Suppress unused parameter warnings
    (void)actor;
    (void)req;

    std::string json =
        JsonBuilder::root_object()
            .object("data")
            .field("version", std::string("v1"))
            .object("endpoints")
            .field("GET /api/v1/actors", std::string("List actors with pagination"))
            .field("GET /api/v1/actors/:id", std::string("Get actor detail"))
            .field("DELETE /api/v1/actors/:id", std::string("Kill an actor"))
            .field("GET /api/v1/actors/:id/mailbox",
                   std::string("Get actor mailbox snapshot"))
            .field("GET /api/v1/actors/:id/children",
                   std::string("Get actor children"))
            .field("GET /api/v1/actors/:id/circuit-breaker",
                   std::string("Get circuit breaker state"))
            .field("POST /api/v1/actors/:id/circuit-breaker/reset",
                   std::string("Reset circuit breaker"))
            .field("POST /api/v1/actors/:id/quarantine",
                   std::string("Quarantine an actor"))
            .field("DELETE /api/v1/actors/:id/quarantine",
                   std::string("Release from quarantine"))
            .field("GET /api/v1/actors/:id/memory",
                   std::string("Get actor memory stats"))
            .field("GET /api/v1/system", std::string("Get system overview"))
            .field("GET /api/v1/system/stats", std::string("Get system statistics"))
            .field("GET /api/v1/system/memory",
                   std::string("Get system memory stats"))
            .field("POST /api/v1/system/drain", std::string("Drain the system"))
            .field("POST /api/v1/system/shutdown",
                   std::string("Shutdown the system"))
            .field("GET /api/v1/faults", std::string("Get fault injection status"))
            .field("POST /api/v1/faults/clear",
                   std::string("Clear fault injection schedule"))
            .field("GET /api/v1/dlq", std::string("List dead letter queue records"))
            .field("GET /api/v1/dlq/:index", std::string("Get a DLQ record"))
            .field("POST /api/v1/dlq/:index/replay",
                   std::string("Replay a DLQ record"))
            .field("GET /api/v1/dlq/export", std::string("Export all DLQ records"))
            .field("GET /api/v1/asks", std::string("List pending asks"))
            .field("GET /api/v1/asks/:message_id", std::string("Get an ask status"))
            .field("DELETE /api/v1/asks/:message_id",
                   std::string("Cancel a pending ask"))
            .end_object()
            .end_object()
            .end_object()
            .build();

    send_json_ok(conn, json);
}

// ====================================================================
// Task 10: handle_get_system
// ====================================================================

void handle_get_system(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req) {
    (void)req;

    auto& sys = actor->system();
    uint64_t total_actors = sys.actor_count();
    uint32_t worker_count = 0;
    if (auto* sched = sys.scheduler()) {
        worker_count = static_cast<uint32_t>(sched->worker_count());
    }

    std::string json = JsonBuilder::root_object()
                           .object("data")
                           .field("total_actors", total_actors)
                           .field("worker_count", worker_count)
                           .field("uptime_ms", static_cast<uint64_t>(0))
                           .end_object()
                           .end_object()
                           .build();

    send_json_ok(conn, json);
}

// ====================================================================
// Task 10: handle_get_system_stats
// ====================================================================

void handle_get_system_stats(CliHttpServerActor* actor,
                             net::HTTPConnection* conn, net::HttpRequest&& req) {
    // Delegates to same logic as handle_get_system for now
    handle_get_system(actor, conn, std::move(req));
}

// ====================================================================
// Task 10: handle_get_system_memory
// ====================================================================

void handle_get_system_memory(CliHttpServerActor* actor,
                              net::HTTPConnection* conn, net::HttpRequest&& req) {
    (void)actor;

    auto jb = JsonBuilder::root_object();
    jb.object("data");

    // Check for per-actor query
    auto actor_id_str = parse_query_string(req, "actor_id");
    if (actor_id_str && !actor_id_str->empty()) {
        // Per-actor memory — return just kActor region
        auto snap =
            mem::MemoryRegionRegistry::instance().snapshot(mem::RegionType::kActor);
        char* end = nullptr;
        uint64_t aid_val = std::strtoull(actor_id_str->c_str(), &end, 10);
        jb.field("actor_id", aid_val);
        jb.field("active_bytes", snap.active_bytes);
        jb.field("peak_bytes", snap.high_water_mark);
        jb.field("segment_count", static_cast<uint64_t>(0));
        jb.field("slab_hit_rate", 0.0);
    } else {
        // System-wide — iterate all region types
        jb.array("regions");
        static constexpr mem::RegionType kRegions[] = {
            mem::RegionType::kActor,     mem::RegionType::kMessage,
            mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
            mem::RegionType::kInternal,  mem::RegionType::kHibernate};

        for (auto region : kRegions) {
            auto snap = mem::MemoryRegionRegistry::instance().snapshot(region);
            jb.object();
            jb.field("name", std::string(mem::to_string(region)));
            jb.field("active_bytes", snap.active_bytes);
            jb.field("peak_bytes", snap.high_water_mark);
            jb.field("alloc_count", snap.alloc_count);
            jb.field("free_count", snap.free_count);
            jb.field("corruption_events", snap.corruption_events);
            jb.field("rejected_alloc_count", snap.rejected_alloc_count);
            jb.field("pressure", std::string(mem::to_string(snap.pressure)));
            jb.field("hard_limit_bytes", snap.limit.hard_limit_bytes);
            jb.end_object();
        }
        jb.end_array();
    }

    jb.end_object(); // data
    send_json_ok(conn, jb.end_object().build());
}

// ====================================================================
// Task 10: handle_drain
// ====================================================================

void handle_drain(CliHttpServerActor* actor, net::HTTPConnection* conn,
                  net::HttpRequest&& req) {
    if (!validate_json_content_type(conn, req))
        return;
    actor->drain();
    send_accepted(conn, "System drain initiated");
}

// ====================================================================
// Task 10: handle_shutdown
// ====================================================================

void handle_shutdown(CliHttpServerActor* actor, net::HTTPConnection* conn,
                     net::HttpRequest&& req) {
    if (!validate_json_content_type(conn, req))
        return;
    actor->shutdown();
    send_accepted(conn, "System shutdown initiated");
}

} // namespace handlers
} // namespace cli
} // namespace hpactor
