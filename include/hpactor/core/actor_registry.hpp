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

#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <unordered_map>

namespace hpactor {

// -----------------------------------------------------------------------------
// actor_registry - maintains a map of actor names to their addresses
// -----------------------------------------------------------------------------
class actor_registry {
  public:
    explicit actor_registry(CommunicationEndpoint endpoint);

    void put(const std::string& name, ActorAddress addr);
    ActorAddress get(const std::string& name) const;
    void erase(const std::string& name);

  private:
    [[maybe_unused]] CommunicationEndpoint endpoint_;
    std::unordered_map<std::string, ActorAddress> actors_;
};

} // namespace hpactor