// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "cli_http_handler_helpers.hpp"

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli/actor/cli_http_server_actor.hpp>
#include <hpactor/cli/http_handler.hpp>
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

namespace hpactor::cli::handlers {

using adt::JsonBuilder;

class GetFaultsHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/faults";

    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        (void)req;
        auto& fc = actor.system().fault_controller();
        send_json_ok(&conn, JsonBuilder::root_object()
                                .object("data")
                                .field("enabled", fc.is_enabled())
                                .field("seed", fc.replay_seed())
                                .field("hooks_triggered", fc.faults_fired())
                                .end_object()
                                .end_object()
                                .build());
    }
};

class ClearFaultsHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::POST;
    static constexpr std::string_view kPath = "/api/v1/faults/clear";

    void handle(CliHttpServerActor& actor, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!validate_json_content_type(&conn, req))
            return;
        actor.system().fault_controller().clear();
        send_success(&conn);
    }
};

void register_fault_handlers() {
    auto& reg = HttpHandlerRegistry::instance();
    reg.add(GetFaultsHandler::kMethod, std::string(GetFaultsHandler::kPath),
            std::make_unique<GetFaultsHandler>());
    reg.add(ClearFaultsHandler::kMethod, std::string(ClearFaultsHandler::kPath),
            std::make_unique<ClearFaultsHandler>());
}

} // namespace hpactor::cli::handlers
