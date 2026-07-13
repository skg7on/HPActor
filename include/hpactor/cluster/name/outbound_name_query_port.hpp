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

#include <cstdint>

#include <hpactor/msg/fwd.hpp>
#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

/// \brief Function-pointer port for sending name-protocol messages to
///        remote nodes through the transport.
///
/// Installed on NameResolver at construction. Fixed-size (one function
/// pointer + one \c void*). When \c send is \c nullptr, outbound messages
/// are silently dropped.
///
/// \note No \c std::function, no exceptions.
struct OutboundNameQueryPort {
    /// \brief Callback signature for sending a TypedMessage to a peer.
    using SendFn = void (*)(void* context, EndPoint target, TypedMessage msg);

    void* context = nullptr; ///< Opaque pointer passed to the callback.
    SendFn send = nullptr;   ///< Outbound send callback.

    /// \brief True when the port has a send callback installed.
    /// \return \c true if \c send is non-null.
    [[nodiscard]] bool active() const noexcept {
        return send != nullptr;
    }
};

} // namespace hpactor::cluster::name
