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
#include <hpactor/cluster/name/consistent_hash_ring.hpp>

#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

namespace {
constexpr uint64_t kFnvPrime = 0x00000100000001B3ULL;
constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ULL;
} // namespace

bool EndPointCompare::operator()(const EndPoint& a,
                                  const EndPoint& b) const noexcept {
    return endpoint_ops::to_string(a) < endpoint_ops::to_string(b);
}

HashToken ConsistentHashRing::hash(std::string_view s) noexcept {
    uint64_t h = kFnvOffsetBasis;
    for (char c : s) {
        h ^= static_cast<uint64_t>(static_cast<uint8_t>(c));
        h *= kFnvPrime;
    }
    return h;
}

HashToken ConsistentHashRing::virtual_node_hash(EndPoint node,
                                                 uint32_t vn) noexcept {
    std::string key = endpoint_ops::to_string(node);
    key += ':';
    key += std::to_string(vn);
    return hash(key);
}

void ConsistentHashRing::build(
    const std::set<EndPoint, EndPointCompare>& live_members,
    uint32_t virtual_nodes) {
    ring_.clear();
    physical_nodes_.clear();

    for (const auto& node : live_members) {
        physical_nodes_.insert(node);
        for (uint32_t vn = 0; vn < virtual_nodes; ++vn) {
            HashToken token = virtual_node_hash(node, vn);
            ring_[token] = node;
        }
    }
}

std::optional<EndPoint>
ConsistentHashRing::lookup(std::string_view name) const {
    if (ring_.empty())
        return std::nullopt;
    HashToken token = hash(name);
    // Find the first ring entry with token >= hash(name).
    auto it = ring_.lower_bound(token);
    if (it == ring_.end()) {
        // Wrap around to the first ring entry.
        it = ring_.begin();
    }
    return it->second;
}

} // namespace hpactor::cluster::name
