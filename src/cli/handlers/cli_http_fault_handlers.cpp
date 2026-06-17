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
#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/net/http_connection.hpp>
#include <hpactor/net/http_types.hpp>

#include <string>

namespace hpactor {
namespace cli {
namespace handlers {

using adt::JsonBuilder;

// ====================================================================
// Task 11: handle_get_faults
// ====================================================================

void handle_get_faults(CliHttpServerActor* actor, net::HTTPConnection* conn,
                       net::HttpRequest&& req) {
    (void)req;

    auto& fc = actor->system().fault_controller();

    std::string json = JsonBuilder::root_object()
                           .object("data")
                           .field("enabled", fc.is_enabled())
                           .field("seed", fc.replay_seed())
                           .field("hooks_triggered", fc.faults_fired())
                           .end_object()
                           .end_object()
                           .build();

    send_json_ok(conn, json);
}

// ====================================================================
// Task 11: handle_clear_faults
// ====================================================================

void handle_clear_faults(CliHttpServerActor* actor, net::HTTPConnection* conn,
                         net::HttpRequest&& req) {
    if (!validate_json_content_type(conn, req))
        return;

    auto& fc = actor->system().fault_controller();
    fc.clear();

    send_success(conn);
}

} // namespace handlers
} // namespace cli
} // namespace hpactor
