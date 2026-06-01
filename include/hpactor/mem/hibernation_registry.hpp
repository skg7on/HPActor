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

#include <hpactor/types/types.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace hpactor::mem {

/// \brief Serialized actor state stored in cold memory during hibernation.
struct HibernationBuffer {
    void* ptr{nullptr};           ///< Owned pointer to the serialized buffer.
    size_t size{0};               ///< Size of the serialized buffer in bytes.
    uint64_t hibernated_at_ts{0}; ///< Monotonic timestamp when hibernation
                                  ///< completed.
    uint32_t actor_id{0};         ///< ActorId that owns this buffer.
};

/// \brief Thread-safe registry mapping ActorId to HibernationBuffer.
///
/// Stores serialized actor state for hibernated actors. Ownership of the
/// buffer memory transfers to the registry on \c store() and back to the
/// caller on \c load().
///
/// \note Externally synchronized via internal mutex; all public methods are
///       safe to call from any thread.
class HibernationRegistry {
  public:
    /// \brief Return the singleton instance.
    static HibernationRegistry& instance();

    /// \brief Store a hibernated actor's serialized buffer.
    ///
    /// Takes ownership of \p buf.ptr. The caller must not free it after
    /// this call.
    ///
    /// \param[in] id Actor identifier.
    /// \param[in] buf Buffer descriptor (ownership transfers to registry).
    void store(ActorId id, HibernationBuffer buf);

    /// \brief Retrieve and remove a hibernated actor's buffer (for
    /// reactivation).
    ///
    /// Ownership of the returned buffer's pointer transfers to the caller.
    ///
    /// \param[in] id Actor identifier.
    /// \return The buffer, with \c ptr == \c nullptr if not found.
    HibernationBuffer load(ActorId id);

    /// \brief Remove an entry without retrieving the buffer.
    ///
    /// Used when a hibernated actor is terminated. The buffer memory is freed.
    ///
    /// \param[in] id Actor identifier.
    void remove(ActorId id);

    /// \brief Check whether an actor is currently hibernated.
    ///
    /// \param[in] id Actor identifier.
    /// \return \c true if a buffer is stored for this actor.
    bool contains(ActorId id) const;

    /// \brief Return the number of hibernated actors.
    ///
    /// \return Total count of stored buffers.
    size_t count() const;

    /// \brief Return the total bytes consumed by all hibernated buffers.
    ///
    /// \return Sum of \c size across all stored buffers.
    size_t total_bytes() const;

  private:
    HibernationRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, HibernationBuffer> entries_;
};

} // namespace hpactor::mem
