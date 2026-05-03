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

#include <hpactor/actor/daemon_actor.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor {

class ExternalMsgGatewayActor : public DaemonActor {
  public:
    ExternalMsgGatewayActor(ActorContext* ctx, ActorSystem& sys)
        : DaemonActor(ctx, sys) {}

    void route(const std::string& path_pattern, ActorAddr target) {
        routes_[path_pattern] = target;
    }

    void route(const std::string& path_pattern, ActorRef target) {
        routes_[path_pattern] = target.address();
    }

    using PayloadTransform =
        std::function<TypedMessage(StreamBuffer)>;

    void set_transform(TypeTag tag, PayloadTransform tx) {
        transforms_[tag] = std::move(tx);
    }

  protected:
    ActorAddr resolve_route(const std::string& path) const {
        auto it = routes_.find(path);
        if (it != routes_.end()) return it->second;

        for (const auto& [pattern, target] : routes_) {
            if (path.find(pattern) == 0) return target;
        }
        return invalid_actor_addr;
    }

    TypedMessage transform(TypeTag tag, StreamBuffer payload) const {
        auto it = transforms_.find(tag);
        if (it != transforms_.end()) {
            return it->second(std::move(payload));
        }
        return TypedMessage(tag, std::move(payload));
    }

    std::unordered_map<std::string, ActorAddr> routes_;
    std::unordered_map<TypeTag, PayloadTransform> transforms_;
};

} // namespace hpactor
