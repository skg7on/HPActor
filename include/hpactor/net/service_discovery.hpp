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

#include <hpactor/adt/node_identity.hpp>
#include <hpactor/net/acceptor.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::net {

enum class MemberStatus : uint8_t { Alive, Suspicious, Dead, Left };

struct Member {
    NodeIdentity identity;
    std::vector<std::string> actor_types;
    MemberStatus status = MemberStatus::Alive;
    uint64_t incarnation = 0;
    // Not transmitted on wire — receivers set to steady_clock::now() on
    // receipt.
    std::chrono::steady_clock::time_point last_seen;
};

using MemberChangeCallback = std::function<void(const Member&, bool joined)>;

class IServiceDiscovery {
  public:
    virtual ~IServiceDiscovery() = default;

    virtual void start() = 0;
    virtual void stop() = 0;

    virtual std::vector<Member> discover_all() const = 0;
    virtual const Member* discover(EndPoint) const = 0;
    virtual void announce(Member local_state) = 0;
    virtual void on_member_change(MemberChangeCallback) = 0;
    virtual std::string backend_name() const = 0;

    virtual const std::unordered_map<EndPoint, Member>* raw_members() const {
        return nullptr;
    }
};

} // namespace hpactor::net
