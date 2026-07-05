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

#include <hpactor/net/network_snapshot.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

namespace net {
class EventLoop;
class Transport;
class UdpRegistrar;
} // namespace net

class NetworkRuntime;
class RpcChannel;

/// \brief Non-owning read-only view of the network subsystem.
///
/// Obtained from \c ActorSystem::network().  Returns \c nullptr for
/// optional resources when networking is disabled.  Lifetime must not
/// exceed the parent \c ActorSystem.
///
/// \note All methods are thread-safe.
class ActorSystemNetworkView final {
  public:
    /// \brief True if networking is enabled and the runtime exists.
    [[nodiscard]] bool is_enabled() const noexcept;

    /// \brief Bounded copy of current network state.
    [[nodiscard]] net::NetworkSnapshot snapshot() const noexcept;

    /// \brief Local endpoint for this node.
    [[nodiscard]] EndPoint endpoint() const noexcept;

    /// \brief The event loop (nullptr if networking disabled).
    [[nodiscard]] net::EventLoop* event_loop() const noexcept;

    /// \brief The primary transport (nullptr if networking disabled).
    [[nodiscard]] net::Transport* transport() const noexcept;

    /// \brief The UDP registrar (nullptr if networking disabled or
    ///        discovery does not use a registrar).
    [[nodiscard]] net::UdpRegistrar* registrar() const noexcept;

  private:
    friend class ActorSystem;

    ActorSystemNetworkView(NetworkRuntime* network, EndPoint endpoint) noexcept;

    NetworkRuntime* network_;
    EndPoint endpoint_;
};

} // namespace hpactor
