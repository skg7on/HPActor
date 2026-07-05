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

#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/mailbox/outbound_tracker.hpp>
#include <hpactor/msg/dead_letter_record.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>

namespace hpactor {

// Forward declaration — MessagingRuntime is a private type.
class MessagingRuntime;

/// \brief Non-owning read-only view of the messaging subsystem.
///
/// Obtained from \c ActorSystem::messaging().  Provides access to
/// delivery tracking, deduplication, and dead-letter state.
/// Lifetime must not exceed the parent \c ActorSystem.
///
/// \note All methods are thread-safe.
class ActorSystemMessagingView final {
  public:
    /// \brief Bounded snapshot of the dead-letter queue.
    [[nodiscard]] mailbox::DeadLetterQueueSnapshot
    dead_letter_snapshot() const noexcept;

    /// \brief The dead-letter queue (nullptr if DLQ is disabled).
    [[nodiscard]] mailbox::DeadLetterQueue* dead_letter_queue() const noexcept;

    /// \brief The outbound delivery tracker for at-least-once delivery.
    [[nodiscard]] msg::OutboundDeliveryTracker* outbound_tracker() const noexcept;

    /// \brief The reliable messaging outbound tracker (ACK/NACK/retry).
    [[nodiscard]] mailbox::OutboundTracker* reliable_tracker() const noexcept;

    /// \brief The receiver dedup cache for at-least-once delivery.
    [[nodiscard]] adt::DedupCache* dedup_cache() const noexcept;

  private:
    friend class ActorSystem;

    explicit ActorSystemMessagingView(MessagingRuntime& messaging) noexcept;

    MessagingRuntime* messaging_;
};

} // namespace hpactor
