// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "cli_http_handler_helpers.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli/actor/cli_http_server_actor.hpp>
#include <hpactor/cli/http_handler.hpp>
#include <hpactor/mem/memory_region.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

#include <string>

namespace hpactor::cli::handlers {

using adt::JsonBuilder;

class ApiIndexHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1";
    void handle(CliHttpServerActor&, net::HTTPConnection& conn,
                net::HttpRequest&&) override {
        send_json_ok(
            &conn,
            JsonBuilder::root_object()
                .object("data")
                .field("version", std::string("v1"))
                .object("endpoints")
                .field("GET /api/v1/actors",
                       std::string("List actors with pagination"))
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
                .field("GET /api/v1/system/stats",
                       std::string("Get system statistics"))
                .field("GET /api/v1/system/memory",
                       std::string("Get system memory stats"))
                .field("POST /api/v1/system/drain", std::string("Drain the system"))
                .field("POST /api/v1/system/shutdown",
                       std::string("Shutdown the system"))
                .field("GET /api/v1/faults", std::string("Get fault injection status"))
                .field("POST /api/v1/faults/clear",
                       std::string("Clear fault injection schedule"))
                .field("GET /api/v1/dlq",
                       std::string("List dead letter queue records"))
                .field("GET /api/v1/dlq/:index", std::string("Get a DLQ record"))
                .field("POST /api/v1/dlq/:index/replay",
                       std::string("Replay a DLQ record"))
                .field("GET /api/v1/dlq/export", std::string("Export all DLQ records"))
                .field("GET /api/v1/asks", std::string("List pending asks"))
                .field("GET /api/v1/asks/:message_id",
                       std::string("Get an ask status"))
                .field("DELETE /api/v1/asks/:message_id",
                       std::string("Cancel a pending ask"))
                .end_object()
                .end_object()
                .end_object()
                .build());
    }
};

class GetSystemHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/system";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&&) override {
        auto& sys = actor.system();
        uint64_t total_actors = sys.actor_count();
        uint32_t worker_count = 0;
        if (auto* sched = sys.scheduler())
            worker_count = static_cast<uint32_t>(sched->worker_count());
        send_json_ok(&conn, JsonBuilder::root_object()
                                .object("data")
                                .field("total_actors", total_actors)
                                .field("worker_count", worker_count)
                                .field("uptime_ms", static_cast<uint64_t>(0))
                                .end_object()
                                .end_object()
                                .build());
    }
};

class GetSystemStatsHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/system/stats";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        GetSystemHandler h;
        h.handle(actor, conn, std::move(req));
    }
};

class GetSystemMemoryHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/system/memory";
    void handle(CliHttpServerActor&, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        auto jb = JsonBuilder::root_object();
        jb.object("data");
        auto actor_id_str = parse_query_string(req, "actor_id");
        if (actor_id_str && !actor_id_str->empty()) {
            auto snap = mem::MemoryRegionRegistry::instance().snapshot(
                mem::RegionType::kActor);
            char* end = nullptr;
            uint64_t aid_val = std::strtoull(actor_id_str->c_str(), &end, 10);
            jb.field("actor_id", aid_val).field("active_bytes", snap.active_bytes);
            jb.field("peak_bytes", snap.high_water_mark)
                .field("segment_count", uint64_t(0));
            jb.field("slab_hit_rate", 0.0);
        } else {
            jb.array("regions");
            static constexpr mem::RegionType kRegions[] = {
                mem::RegionType::kActor,     mem::RegionType::kMessage,
                mem::RegionType::kCoroutine, mem::RegionType::kNetwork,
                mem::RegionType::kInternal,  mem::RegionType::kHibernate};
            auto& reg = mem::MemoryRegionRegistry::instance();
            for (auto region : kRegions) {
                auto snap = reg.snapshot(region);
                jb.object()
                    .field("name", std::string(mem::to_string(region)))
                    .field("active_bytes", snap.active_bytes)
                    .field("peak_bytes", snap.high_water_mark)
                    .field("alloc_count", snap.alloc_count)
                    .field("free_count", snap.free_count)
                    .field("corruption_events", snap.corruption_events)
                    .field("rejected_alloc_count", snap.rejected_alloc_count)
                    .field("pressure", std::string(mem::to_string(snap.pressure)))
                    .field("hard_limit_bytes", snap.limit.hard_limit_bytes)
                    .end_object();
            }
            jb.end_array();
        }
        jb.end_object();
        send_json_ok(&conn, jb.end_object().build());
    }
};

class DrainHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::POST;
    static constexpr std::string_view kPath = "/api/v1/system/drain";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!validate_json_content_type(&conn, req))
            return;
        actor.drain();
        send_accepted(&conn, "System drain initiated");
    }
};

class ShutdownHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::POST;
    static constexpr std::string_view kPath = "/api/v1/system/shutdown";
    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!validate_json_content_type(&conn, req))
            return;
        actor.shutdown();
        send_accepted(&conn, "System shutdown initiated");
    }
};

void register_system_handlers() {
    auto& r = HttpHandlerRegistry::instance();
    r.add(ApiIndexHandler::kMethod, std::string(ApiIndexHandler::kPath),
          std::make_unique<ApiIndexHandler>());
    r.add(GetSystemHandler::kMethod, std::string(GetSystemHandler::kPath),
          std::make_unique<GetSystemHandler>());
    r.add(GetSystemStatsHandler::kMethod, std::string(GetSystemStatsHandler::kPath),
          std::make_unique<GetSystemStatsHandler>());
    r.add(GetSystemMemoryHandler::kMethod,
          std::string(GetSystemMemoryHandler::kPath),
          std::make_unique<GetSystemMemoryHandler>());
    r.add(DrainHandler::kMethod, std::string(DrainHandler::kPath),
          std::make_unique<DrainHandler>());
    r.add(ShutdownHandler::kMethod, std::string(ShutdownHandler::kPath),
          std::make_unique<ShutdownHandler>());
}

} // namespace hpactor::cli::handlers
