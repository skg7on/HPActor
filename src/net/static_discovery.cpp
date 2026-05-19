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

#include <hpactor/net/static_discovery.hpp>

namespace hpactor::net {

StaticDiscovery::StaticDiscovery(std::vector<Member> members)
    : members_(std::move(members)) {
    for (size_t i = 0; i < members_.size(); ++i) {
        index_[members_[i].identity.endpoint] = i;
    }
}

std::vector<Member> StaticDiscovery::discover_all() const {
    return members_;
}

const Member* StaticDiscovery::discover(EndPoint ep) const {
    auto it = index_.find(ep);
    if (it != index_.end())
        return &members_[it->second];
    return nullptr;
}

} // namespace hpactor::net
