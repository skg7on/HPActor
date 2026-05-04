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

#include <hpactor/config/actor_factory.hpp>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::config {

// -----------------------------------------------------------------------------
// ActorFactoryRegistry — singleton registry mapping behavior name strings to
// ActorFactory functions.
//
// Populated at static init time via HPACTOR_REGISTER_ACTOR. Read-only after
// main() starts (no runtime registration). Thread-safe by immutability.
// -----------------------------------------------------------------------------
class ActorFactoryRegistry {
  public:
    static ActorFactoryRegistry& instance();

    template <typename T>
    void register_factory(const std::string& name);

    ActorFactory get_factory(const std::string& name) const;
    bool has(const std::string& name) const;
    std::vector<std::string> known_names() const;

  private:
    std::unordered_map<std::string, ActorFactory> factories_;
};

// Template implementation
template <typename T>
void ActorFactoryRegistry::register_factory(const std::string& name) {
    factories_.emplace(name, [](ActorContext* ctx, ActorSystem& sys) {
        return std::make_shared<T>(ctx, sys);
    });
}

} // namespace hpactor::config

// -----------------------------------------------------------------------------
// HPACTOR_REGISTER_ACTOR — static registration macro
//
// Place in an actor's .cpp file to register it for TOML-based bootstrapping.
// Registration happens before main() via static initialization.
//
// Example:
//   HPACTOR_REGISTER_ACTOR("TcpGatewayActor", TcpGatewayActor);
// -----------------------------------------------------------------------------
#define HPACTOR_REGISTER_ACTOR(Name, ActorClass)                               \
    namespace {                                                                  \
        [[maybe_unused]] static const bool _hpactor_reg_##ActorClass = [] {      \
            ::hpactor::config::ActorFactoryRegistry::instance()                  \
                .register_factory<ActorClass>(Name);                            \
            return true;                                                         \
        }();                                                                     \
    }
