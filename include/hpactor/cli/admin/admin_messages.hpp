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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::cli::admin {

/// \brief Admin API resource path enumeration.
enum class AdminResource : uint8_t {
    Actors,       ///< GET /admin/actors
    ClusterNodes, ///< GET /admin/cluster/nodes
    Health,       ///< GET /admin/health
    Shutdown,     ///< POST /admin/shutdown
};

/// \brief Request to an admin endpoint.
struct AdminRequest {
    AdminResource resource; ///< Which endpoint is being requested.
    std::string body;       ///< Optional JSON body (for POST).
};

/// \brief Response from an admin endpoint.
struct AdminResponse {
    uint16_t status_code = 200; ///< HTTP status code.
    std::string body;           ///< JSON response body.
};

} // namespace hpactor::net::admin
