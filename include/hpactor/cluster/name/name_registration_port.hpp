// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <string_view>

#include <hpactor/ref/actor_address.hpp>

namespace hpactor::cluster::name {

/// \brief Function-pointer port installed on ActorDirectory for name
///        registration/unregistration callbacks.
///
/// Fixed-size (two pointers + one void*). No std::function, no exceptions.
struct NameRegistrationPort {
    using RegisterFn = void (*)(void* context, std::string_view name,
                                 ActorAddress address, uint64_t generation);
    using UnregisterFn = void (*)(void* context, std::string_view name);

    void* context = nullptr;
    RegisterFn on_register = nullptr;
    UnregisterFn on_unregister = nullptr;

    /// \brief True when both callbacks are installed.
    [[nodiscard]] bool active() const noexcept {
        return on_register != nullptr && on_unregister != nullptr;
    }
};

} // namespace hpactor::cluster::name
