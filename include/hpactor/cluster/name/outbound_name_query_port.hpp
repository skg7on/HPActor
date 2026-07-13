// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

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
