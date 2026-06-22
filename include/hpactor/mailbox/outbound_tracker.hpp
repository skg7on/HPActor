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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/mailbox/reliable_retry_policy.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

#include <chrono>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace hpactor::mailbox {

/// \brief Monotonic clock for time measurements in reliable delivery.
///
/// Wraps \c std::chrono::steady_clock to provide a steady, monotonic
/// time source unaffected by system clock adjustments.
struct MonotonicClock {
    using time_point = std::chrono::steady_clock::time_point;
    using duration = std::chrono::milliseconds;
    static time_point now() noexcept {
        return std::chrono::steady_clock::now();
    }
};

/// \brief Entry for a single tracked outbound message.
struct OutboundTrackerEntry {
    MessageId message_id;
    ActorAddress target;
    StreamBuffer payload;
    uint32_t retry_count = 0;
    MonotonicClock::time_point next_retry_at;
    MonotonicClock::time_point deadline;
};

/// \brief Tracks pending outbound messages with retry and expiry support.
///
/// Provides per-destination bounded tracking of outbound messages.
/// Supports ACK removal, NACK-based rescheduling with retry policy,
/// deadline-based expiry, and node-level failure draining.
///
/// Thread-safe via internal mutex.
class OutboundTracker {
  public:
    /// \brief Maximum pending messages per destination node.
    static constexpr size_t kMaxPendingPerDestination = 1024;

    /// \brief Construct with a retry policy.
    explicit OutboundTracker(ReliableRetryPolicy policy);

    /// \brief Track a new outbound message.
    ///
    /// \param msg_id  The message identifier.
    /// \param target  The target actor address.
    /// \param payload The message payload.
    /// \param deadline The absolute deadline for delivery (default: infinite).
    /// \return true if the message was tracked, false if capacity exceeded.
    bool
    track(MessageId msg_id, ActorAddress target, StreamBuffer payload,
          MonotonicClock::time_point deadline = MonotonicClock::time_point::max());

    /// \brief Remove a tracked message on successful delivery.
    void on_ack(MessageId msg_id);

    /// \brief Handle a negative acknowledgment with retry scheduling.
    ///
    /// If the retry policy allows another attempt, the entry is rescheduled.
    /// Otherwise it is moved to the expired list.
    void on_nack(MessageId msg_id, Duration retry_after);

    /// \brief Advance time for deadline and retry checks.
    ///
    /// Expired entries (past deadline or exceeded retries) are moved
    /// to the internal expired list, retrievable via \c drain_expired().
    void tick(MonotonicClock::time_point now);

    /// \brief Fail all pending messages for a given node.
    ///
    /// All entries whose target node matches \p node_id are moved
    /// to the expired list.
    void fail_pending_for_node(const std::string& node_id);

    /// \brief Return the number of entries still being tracked.
    size_t pending_count() const;

    /// \brief Drain all expired entries.
    ///
    /// Returns and clears the internal expired list.
    std::vector<OutboundTrackerEntry> drain_expired();

  private:
    ReliableRetryPolicy policy_;
    std::unordered_map<uint64_t, OutboundTrackerEntry> entries_;
    std::unordered_map<std::string, size_t> per_dest_count_;
    std::vector<OutboundTrackerEntry> expired_;
    mutable std::mutex mutex_;
};

} // namespace hpactor::mailbox
