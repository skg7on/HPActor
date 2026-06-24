// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include "cli_http_handler_helpers.hpp"

#include <hpactor/cli/actor/cli_http_server_actor.hpp>
#include <hpactor/cli/http_handler.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

namespace hpactor::cli::handlers {

class ListAsksHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/asks";
    void handle(CliHttpServerActor&, net::HTTPConnection& conn,
                net::HttpRequest&&) override {
        send_error(&conn, net::HttpStatusCode::NotImplemented,
                   "NOT_IMPLEMENTED", "Ask enumeration not yet available");
    }
};

class GetAskHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::GET;
    static constexpr std::string_view kPath = "/api/v1/asks/:message_id";
    void handle(CliHttpServerActor&, net::HTTPConnection& conn,
                net::HttpRequest&&) override {
        send_error(&conn, net::HttpStatusCode::NotFound, "ASK_NOT_FOUND",
                   "Ask lookup by message_id is not yet implemented");
    }
};

class CancelAskHandler final : public IHttpHandler {
  public:
    static constexpr net::HttpMethod kMethod = net::HttpMethod::DELETE;
    static constexpr std::string_view kPath = "/api/v1/asks/:message_id";
    void handle(CliHttpServerActor&, net::HTTPConnection& conn,
                net::HttpRequest&& req) override {
        if (!validate_json_content_type(&conn, req))
            return;
        send_error(&conn, net::HttpStatusCode::NotImplemented,
                   "NOT_IMPLEMENTED", "Ask cancellation not yet available");
    }
};

void register_ask_handlers() {
    auto& r = HttpHandlerRegistry::instance();
    r.add(ListAsksHandler::kMethod, std::string(ListAsksHandler::kPath),
          std::make_unique<ListAsksHandler>());
    r.add(GetAskHandler::kMethod, std::string(GetAskHandler::kPath),
          std::make_unique<GetAskHandler>());
    r.add(CancelAskHandler::kMethod, std::string(CancelAskHandler::kPath),
          std::make_unique<CancelAskHandler>());
}

} // namespace hpactor::cli::handlers
