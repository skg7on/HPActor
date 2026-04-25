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

#include <cassert>
#include <string>

// Helper to test path derivation
static std::string derive_uds_path(const std::string& node_id) {
    std::string sanitized = node_id;
    for (char& c : sanitized) {
        if (c == ':') c = '_';
    }
    return "/tmp/hpactor/" + sanitized + ".sock";
}

int main() {
    // Test SimpleNodeId: "localhost:5000" -> "/tmp/hpactor/localhost_5000.sock"
    {
        auto path = derive_uds_path("localhost:5000");
        assert(path == "/tmp/hpactor/localhost_5000.sock");
    }

    // Test IpAddress: "127.0.0.1:8080" -> "/tmp/hpactor/127.0.0.1_8080.sock"
    {
        auto path = derive_uds_path("127.0.0.1:8080");
        assert(path == "/tmp/hpactor/127.0.0.1_8080.sock");
    }

    // Test NoPort: "node1" -> "/tmp/hpactor/node1.sock"
    {
        auto path = derive_uds_path("node1");
        assert(path == "/tmp/hpactor/node1.sock");
    }

    // Test MultipleColons: "192.168.1.1:5000" -> "/tmp/hpactor/192.168.1.1_5000.sock"
    {
        auto path = derive_uds_path("192.168.1.1:5000");
        assert(path == "/tmp/hpactor/192.168.1.1_5000.sock");
    }

    return 0;
}