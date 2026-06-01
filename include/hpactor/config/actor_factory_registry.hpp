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

/// \brief Singleton registry mapping behavior name strings to ActorFactory
///        functions.
///
/// Populated at static init time via HPACTOR_REGISTER_ACTOR. Read-only after
/// main() starts (no runtime registration). Thread-safe by immutability.
///
/// \note Registration phase: static initialization (single-threaded).
///       Lookup phase: read-only, safe for concurrent access.
class ActorFactoryRegistry {
  public:
    /// \brief Access the singleton instance.
    ///
    /// \return Reference to the process-global registry.
    static ActorFactoryRegistry& instance();

    /// \brief Register a factory for the given behavior name.
    ///
    /// \tparam T Actor subclass with a constructor taking (ActorContext*,
    ///           ActorSystem&).
    /// \param[in] name Behavior name string used in TOML actor definitions.
    template <typename T> void register_factory(const std::string& name);

    /// \brief Look up a factory by behavior name.
    ///
    /// \param[in] name Behavior name.
    /// \return The factory function, or an empty (default-constructed)
    ///         ActorFactory if not found.
    ActorFactory get_factory(const std::string& name) const;

    /// \brief Check whether a behavior name is registered.
    ///
    /// \param[in] name Behavior name.
    /// \retval true A factory is registered for this name.
    /// \retval false No factory is registered.
    bool has(const std::string& name) const;

    /// \brief All registered behavior names.
    ///
    /// \return Sorted list of known behavior names.
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

/// \brief Static registration macro for TOML-based actor bootstrapping.
///
/// Place in an actor's .cpp file to register it for TOML topology
/// configuration. Registration happens before main() via static
/// initialization.
///
/// \code{.cpp}
/// HPACTOR_REGISTER_ACTOR("TcpGatewayActor", TcpGatewayActor);
/// \endcode
///
/// \param Name String literal — the behavior name used in TOML.
/// \param ActorClass The C++ actor class to instantiate.
#define HPACTOR_REGISTER_ACTOR(Name, ActorClass)                               \
    namespace {                                                                \
    [[maybe_unused]] static const bool _hpactor_reg_##ActorClass = [] {        \
        ::hpactor::config::ActorFactoryRegistry::instance()                    \
            .register_factory<ActorClass>(Name);                               \
        return true;                                                           \
    }();                                                                       \
    }
