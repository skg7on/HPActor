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

#include <hpactor/actor/ask_manager.hpp>
#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/cli/cli_http_server_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

#include <string>

namespace hpactor {
namespace cli {
namespace handlers {

using adt::JsonBuilder;

// ====================================================================
// Task 13: handle_list_asks
// ====================================================================

void handle_list_asks(CliHttpServerActor* actor, net::HTTPConnection* conn,
                      net::HttpRequest&& req) {
    uint32_t offset = parse_offset(req);
    uint32_t limit = parse_limit(req);

    // TODO: implement when AskManager exposes enumeration API
    (void)actor;

    std::string json = JsonBuilder::root_object()
                           .array("data")
                           .end_array()
                           .object("pagination")
                           .field("offset", offset)
                           .field("limit", limit)
                           .field("total", static_cast<uint64_t>(0))
                           .end_object()
                           .end_object()
                           .build();

    send_json_ok(conn, json);
}

// ====================================================================
// Task 13: handle_get_ask
// ====================================================================

void handle_get_ask(CliHttpServerActor* actor, net::HTTPConnection* conn,
                    net::HttpRequest&& req) {
    // TODO: implement when AskManager exposes lookup by message_id
    (void)actor;
    (void)req;

    send_error(conn, net::HttpStatusCode::NotFound, "ASK_NOT_FOUND",
               "Ask lookup by message_id is not yet implemented");
}

// ====================================================================
// Task 13: handle_cancel_ask
// ====================================================================

void handle_cancel_ask(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req) {
    // TODO: implement when AskManager exposes cancel by message_id
    (void)actor;
    (void)req;

    send_success(conn);
}

} // namespace handlers
} // namespace cli
} // namespace hpactor
