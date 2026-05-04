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

#include <hpactor/config/actor_factory_registry.hpp>

namespace hpactor::config {

ActorFactoryRegistry& ActorFactoryRegistry::instance() {
    static ActorFactoryRegistry registry;
    return registry;
}

ActorFactory ActorFactoryRegistry::get_factory(const std::string& name) const {
    auto it = factories_.find(name);
    if (it != factories_.end()) {
        return it->second;
    }
    return nullptr;
}

bool ActorFactoryRegistry::has(const std::string& name) const {
    return factories_.find(name) != factories_.end();
}

std::vector<std::string> ActorFactoryRegistry::known_names() const {
    std::vector<std::string> names;
    names.reserve(factories_.size());
    for (const auto& [name, _] : factories_) {
        names.push_back(name);
    }
    return names;
}

} // namespace hpactor::config
