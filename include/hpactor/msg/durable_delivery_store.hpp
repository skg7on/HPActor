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

#include <cstdint>
#include <vector>

namespace hpactor::msg {

/// \brief Simple record for durable persistent send tracking.
///
/// Stored in the outbox before the network send and removed after
/// the remote side acknowledges. Survives process restart so pending
/// sends can be replayed.
struct PendingSend {
    MessageId message_id; ///< Unique message identifier.
};

/// \brief Persistence adapter for durable at-least-once delivery.
///
/// Implementations persist outbox records before network send and inbox
/// records before ACK, so pending sends survive process restart.
///
/// No adapters are implemented in this release — \c DurableAtLeastOnce
/// degrades to in-memory \c AtLeastOnce behavior until a concrete
/// adapter (e.g. \c InMemoryDeliveryStore, \c FileDeliveryStore) is
/// provided in a follow-up.
class DurableDeliveryStore {
  public:
    virtual ~DurableDeliveryStore() = default;

    /// \brief Store a pending outbound message.
    ///
    /// Must be durable (fsync'd or equivalent) before returning.
    ///
    /// \param[in] record The pending send to persist.
    /// \return \c outcome::ok() on success, or an error code.
    virtual result<void> put_outbox(const PendingSend& record) = 0;

    /// \brief Mark an outbox message as acknowledged.
    ///
    /// Removes the record from durable storage.
    ///
    /// \param[in] id The message id to mark complete.
    /// \return \c outcome::ok() on success.
    virtual result<void> mark_outbox_complete(MessageId id) = 0;

    /// \brief Load all unacknowledged outbox messages after restart.
    ///
    /// Called at startup to replay pending sends from durable storage.
    ///
    /// \return A vector of pending sends, or an error.
    virtual result<std::vector<PendingSend>> load_pending_outbox() = 0;

    /// \brief Record an inbound message id for durable dedup.
    ///
    /// \param[in] id      The message id to record.
    /// \param[in] ttl_ns  Time-to-live for the dedup entry in nanoseconds.
    /// \return \c outcome::ok() on success.
    virtual result<void> put_inbox(MessageId id, uint64_t ttl_ns) = 0;

    /// \brief Check whether a message has already been received.
    ///
    /// \param[in] id The message id to check.
    /// \return \c true if the message was already seen within the TTL window.
    virtual result<bool> seen_inbox(MessageId id) = 0;
};

} // namespace hpactor::msg
