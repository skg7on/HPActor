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
#include <span>

namespace hpactor::mem {

// Interface for actors that support hibernation.
// Actors implementing this can be serialized to a compact buffer,
// released from hot memory, and later deserialized back.
class Hibernatable {
  public:
    virtual ~Hibernatable() = default;

    // Return the serialized size of this actor's state.
    // Must be constant for the actor's lifetime.
    virtual size_t serialized_size() const = 0;

    // Serialize actor state into the provided buffer.
    // Buffer is guaranteed to be at least serialized_size() bytes.
    virtual void serialize_to(std::span<std::byte> buffer) const = 0;

    // Deserialize actor state from the provided buffer.
    // Called after the actor's hot memory has been allocated.
    virtual void deserialize_from(std::span<const std::byte> buffer) = 0;
};

} // namespace hpactor::mem
