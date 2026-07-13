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

/// \brief Function-pointer port installed on ActorDirectory for name
///        registration/unregistration callbacks.
///
/// Fixed-size (two function pointers + one \c void*). No \c std::function,
/// no exceptions. When either callback is \c nullptr, the corresponding
/// operation is a no-op.
///
/// \note The port is set once at construction time by \c RuntimeBuilder.
///       Individual call sites check each callback independently — it is
///       valid to set only \c on_register or only \c on_unregister.
struct NameRegistrationPort {
    /// \brief Callback signature for name registration.
    using RegisterFn = void (*)(void* context, std::string_view name,
                                 ActorAddress address, uint64_t generation);
    /// \brief Callback signature for name unregistration.
    using UnregisterFn = void (*)(void* context, std::string_view name);

    void* context = nullptr;       ///< Opaque pointer passed to each callback.
    RegisterFn on_register = nullptr;     ///< Registration callback.
    UnregisterFn on_unregister = nullptr; ///< Unregistration callback.

    /// \brief True when both callbacks are installed.
    /// \return \c true if both \c on_register and \c on_unregister are
    ///         non-null.
    [[nodiscard]] bool active() const noexcept {
        return on_register != nullptr && on_unregister != nullptr;
    }
};

} // namespace hpactor::cluster::name
