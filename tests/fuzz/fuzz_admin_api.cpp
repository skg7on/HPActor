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

/// \file fuzz_admin_api.cpp
/// \brief Fuzz target for \c AdminApiActor::handle() — the admin API dispatch
///        layer for health, actor listing, cluster nodes, and shutdown.

#include "fuzz_harness.hpp"
#include <hpactor/cli/admin/admin_api_actor.hpp>
#include <hpactor/cli/admin/admin_messages.hpp>

#include <string>

using namespace hpactor;
using namespace hpactor::cli::admin;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        if (s < 1)
            return;

        // Use first byte as resource selector
        uint8_t resource_byte = d[0];
        std::string body(reinterpret_cast<const char*>(d + 1), s - 1);

        AdminResource resource;
        switch (resource_byte % 8) {
            case 0:
                resource = AdminResource::Health;
                break;
            case 1:
                resource = AdminResource::Actors;
                break;
            case 2:
                resource = AdminResource::ClusterNodes;
                break;
            case 3:
                resource = AdminResource::Shutdown;
                break;
            // 4-7: out-of-range values — test default case
            default:
                resource = static_cast<AdminResource>(resource_byte);
                break;
        }

        AdminRequest req{resource, std::move(body)};

        // Test 1: nullptr ActorSystem (all handlers check for null)
        {
            cli::admin::AdminApiActor admin(nullptr);
            auto resp = admin.handle(req);
            (void)resp.status_code;
            (void)resp.body.size();
        }

        // Test 2: Register custom handler and exercise dispatch
        {
            cli::admin::AdminApiActor admin(nullptr);
            admin.register_handler(AdminResource::Health,
                                   [](const AdminRequest& r) -> AdminResponse {
                                       return {200, r.body};
                                   });
            auto resp = admin.handle(req);
            (void)resp.status_code;
            (void)resp.body.size();
        }

        // Test 3: Exercise all built-in handlers individually
        {
            cli::admin::AdminApiActor admin(nullptr);
            admin.health_check();
            admin.list_actors();
            admin.list_cluster_nodes();
            admin.do_shutdown();
        }
    });
}
