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

#include <hpactor/core/actor_id.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

namespace hpactor {

/// \brief Both stream routes returned by \c take().
struct StreamRoutes {
    std::optional<ActorId> sender;   ///< Sender actor id, if registered.
    std::optional<ActorId> receiver; ///< Receiver actor id, if registered.
};

/// \brief Thread-safe registry of stream-to-actor mappings.
///
/// Owns the sender and receiver maps for stream routing. All public methods
/// acquire a dedicated mutex; the lock is released before any delivery or
/// callback.
///
/// \note Thread safety: Safe to call from network, actor, and external threads.
class StreamRegistry {
  public:
    /// \brief Register a stream sender for inbound ack routing.
    void register_sender(uint64_t stream_id, ActorId actor_id);

    /// \brief Register a stream receiver for inbound data routing.
    void register_receiver(uint64_t stream_id, ActorId actor_id);

    /// \brief Find the sender actor for a stream.
    std::optional<ActorId> find_sender(uint64_t stream_id) const;

    /// \brief Find the receiver actor for a stream.
    std::optional<ActorId> find_receiver(uint64_t stream_id) const;

    /// \brief Atomically remove and return both routes for a stream.
    StreamRoutes take(uint64_t stream_id);

    /// \brief Current number of registered senders.
    std::size_t sender_count() const;

    /// \brief Current number of registered receivers.
    std::size_t receiver_count() const;

  private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, ActorId> senders_;
    std::unordered_map<uint64_t, ActorId> receivers_;
};

} // namespace hpactor
