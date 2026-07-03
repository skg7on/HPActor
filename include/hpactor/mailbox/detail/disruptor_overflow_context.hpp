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

#include <hpactor/mailbox/detail/reservation_manager.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/msg/enqueue_result.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>

namespace hpactor::mailbox {
class DeadLetterQueue;
}

namespace hpactor::mailbox::detail {

/// \brief Context passed to disruptor overflow handlers during capacity
///        reservation failure.
///
/// Provides the handler with references to the envelope, mailbox state,
/// counters, and callbacks for ring eviction (drop-oldest, drop-lowest-
/// priority). The handler reads from and writes to these fields to produce
/// an \c EnqueueResult.
///
/// \tparam Messages The closed set of fixed user-message types.
template <typename EnvelopeType> struct DisruptorOverflowContext {
    /// The envelope that could not be enqueued (contains message + meta).
    const EnvelopeType& envelope;

    /// Reservation manager for capacity accounting.
    ReservationManager<EnvelopeType>& reservation;

    /// Spill-overflow queue for \c SpillToOverflowQueue policy.
    OverflowQueue<EnvelopeType>& overflow_queue;

    /// Cumulative rejected counter (incremented by handler).
    std::atomic<uint64_t>& total_rejected;

    /// Cumulative dropped counter (incremented by handler).
    std::atomic<uint64_t>& total_dropped;

    /// Cumulative dead-letter counter (incremented by handler).
    std::atomic<uint64_t>& total_dead_letters;

    /// Current mailbox configuration.
    MailboxConfig& config;

    /// Owning actor identifier.
    ActorId actor_id;

    /// Mailbox depth at overflow time.
    uint32_t current_depth;

    /// Queued bytes at overflow time.
    uint64_t current_bytes;

    /// Callback to evict the oldest user message from the highest-priority
    /// non-empty ring. Returns true if a message was dropped.
    std::function<bool()> drop_oldest_fn;

    /// Callback to evict the lowest-priority user message from the
    /// lowest-priority non-empty ring. Returns true if a message was dropped.
    std::function<bool()> drop_lowest_priority_fn;

    /// Optional dead-letter queue pointer (may be null).
    mailbox::DeadLetterQueue* dlq = nullptr;

    /// Mutex for producer blocking (BlockWhenAllowed policy).
    std::mutex* block_mutex = nullptr;

    /// Condition variable for producer blocking (BlockWhenAllowed policy).
    std::condition_variable* block_cv = nullptr;
};

} // namespace hpactor::mailbox::detail
