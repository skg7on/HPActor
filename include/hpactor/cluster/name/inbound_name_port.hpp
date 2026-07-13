// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

/// \brief Function-pointer port for inbound name-protocol messages.
///
/// Installed on InboundFrameRouter. When non-null, frames with name-protocol
/// TypeTags (0x80-0x84) are dispatched here instead of through
/// MessagingRuntime.
struct InboundNamePort {
    using RegisterFn = void (*)(void* context, EndPoint from,
                                std::string_view name, ActorAddress address,
                                uint64_t generation);
    using ResolveFn = void (*)(void* context, EndPoint from,
                               std::string_view name);
    using UnregisterFn = void (*)(void* context, EndPoint from,
                                  std::string_view name, uint64_t generation);

    void* context = nullptr;
    RegisterFn on_register_request = nullptr;
    ResolveFn on_resolve_query = nullptr;
    UnregisterFn on_unregister_request = nullptr;

    [[nodiscard]] bool active() const noexcept {
        return context != nullptr;
    }
};

} // namespace hpactor::cluster::name
