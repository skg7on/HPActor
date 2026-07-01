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

#include <cstddef>
#include <cstdint>

namespace hpactor::mailbox {

/// \brief Discriminator for the actor's mailbox backend.
///
/// Stored in \c ActorDirectoryEntry so the scheduler, delivery engine,
/// and directory lookup paths can select the correct backend without
/// RTTI, \c dynamic_cast, or virtual dispatch.
///
/// \note Values are stable. The default is \c VariableMpsc so existing
///       entries do not require a migration.
enum class MailboxKind : uint8_t {
    /// Default general-purpose mailbox backed by
    /// \c MPSCActorMailbox<TypedMessage>.
    VariableMpsc = 0,

    /// Experimental fixed-message Disruptor-style MPSC ring.
    /// Only set when \c HPACTOR_ENABLE_FIXED_DISRUPTOR_MAILBOX is 1.
    FixedDisruptor = 1,
};

/// \brief Maximum allowed fixed-ring capacity (1 Mi slots).
inline constexpr size_t kMaxFixedRingCapacity = 1u << 20;

/// \brief Compile-time power-of-two capacity check.
///
/// \param[in] capacity The requested ring capacity.
/// \retval true  \p capacity is in [2, kMaxFixedRingCapacity] and is
///               a power of two.
/// \retval false The capacity is out of range or not a power of two.
[[nodiscard]] consteval bool valid_fixed_ring_capacity(size_t capacity) {
    return capacity >= 2 && capacity <= kMaxFixedRingCapacity &&
           (capacity & (capacity - 1)) == 0;
}

} // namespace hpactor::mailbox
