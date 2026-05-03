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

// Hibernation buffer: serialized actor state stored in cold memory.
struct HibernationBuffer {
    void* ptr{nullptr};
    size_t size{0};
    uint64_t hibernated_at_ts{0};
    uint32_t actor_id{0};
};

// Thread-safe registry mapping ActorId → HibernationBuffer.
// Stores serialized actor state for hibernated actors.
class HibernationRegistry {
  public:
    static HibernationRegistry& instance();

    // Store a hibernated actor's buffer. Takes ownership of ptr.
    void store(ActorId id, HibernationBuffer buf);

    // Retrieve and remove (on reactivation). Returns buffer with nullptr if not found.
    HibernationBuffer load(ActorId id);

    // Remove without retrieving (on actor termination while hibernated).
    void remove(ActorId id);

    // Check if an actor is hibernated.
    bool contains(ActorId id) const;

    // Total number of hibernated actors.
    size_t count() const;

    // Total hibernated bytes across all actors.
    size_t total_bytes() const;

  private:
    HibernationRegistry() = default;

    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, HibernationBuffer> entries_;
};

} // namespace hpactor::mem
