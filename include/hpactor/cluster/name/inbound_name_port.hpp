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
#include <string_view>

#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

/// \brief Function-pointer port for inbound name-protocol messages.
///
/// Installed on InboundFrameRouter. When active, frames with name-protocol
/// TypeTags (0x80–0x84) are dispatched here instead of through
/// \c MessagingRuntime. Request tags (0x80, 0x82, 0x84) are
/// short-circuited; response tags (0x81, 0x83) pass through normal delivery.
///
/// \note Fixed-size (three function pointers + one \c void*). No
///       \c std::function, no exceptions.
struct InboundNamePort {
    /// \brief Callback signature for inbound NameRegisterRequest (0x80).
    using RegisterFn = void (*)(void* context, EndPoint from,
                                std::string_view name, ActorAddress address,
                                uint64_t generation);
    /// \brief Callback signature for inbound NameResolveQuery (0x82).
    using ResolveFn = void (*)(void* context, EndPoint from,
                               std::string_view name);
    /// \brief Callback signature for inbound NameUnregisterRequest (0x84).
    using UnregisterFn = void (*)(void* context, EndPoint from,
                                  std::string_view name, uint64_t generation);

    void* context = nullptr;                     ///< Opaque pointer passed to
                                                  ///< each callback.
    RegisterFn on_register_request = nullptr;     ///< Register-request handler.
    ResolveFn on_resolve_query = nullptr;         ///< Resolve-query handler.
    UnregisterFn on_unregister_request = nullptr; ///< Unregister-request handler.

    /// \brief True when the port has a context and at least one handler.
    /// \return \c true if \c context is non-null.
    [[nodiscard]] bool active() const noexcept {
        return context != nullptr;
    }
};

} // namespace hpactor::cluster::name
