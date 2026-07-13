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
/// Installed on NameResolver at construction. Fixed-size (two pointers).
struct OutboundNameQueryPort {
    using SendFn = void (*)(void* context, EndPoint target, TypedMessage msg);

    void* context = nullptr;
    SendFn send = nullptr;

    [[nodiscard]] bool active() const noexcept {
        return send != nullptr;
    }
};

} // namespace hpactor::cluster::name
