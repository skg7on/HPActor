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
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/enqueue_result.hpp>

#include <atomic>
#include <functional>

namespace hpactor::mailbox {
class DeadLetterQueue;
}

namespace hpactor::mailbox::detail {

/// \brief Context passed to overflow handlers during capacity reservation
///        failure.
///
/// Provides the handler with references to the current message, mailbox
/// state, counters, and callbacks for eviction (drop-oldest, drop-lowest-
/// priority). The handler reads from and writes to these fields to produce
/// an \c EnqueueResult.
///
/// \tparam T Message type stored in the mailbox.
template <typename T> struct OverflowContext {
    const T& message;          ///< The message that could not be enqueued.
    MailboxEnvelopeMeta& meta; ///< Envelope metadata (mutable for payload
                               ///< trimming).
    ReservationManager<T>& reservation;    ///< Reservation manager for capacity
                                           ///< accounting.
    OverflowQueue<T>& overflow_queue;      ///< Spill-overflow queue for \c
                                           ///< SpillToOverflowQueue policy.
    std::atomic<uint64_t>& total_rejected; ///< Cumulative rejected counter
                                           ///< (incremented by handler).
    std::atomic<uint64_t>& total_dropped;  ///< Cumulative dropped counter.
    std::atomic<uint64_t>& total_dead_letters; ///< Cumulative dead-letter
                                               ///< counter.
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_buf; ///< Optional
                                                                ///< metrics
                                                                ///< ring
                                                                ///< buffer.
    MailboxConfig& config;                ///< Current mailbox configuration.
    ActorId actor_id;                     ///< Owning actor identifier.
    uint32_t current_depth;               ///< Mailbox depth at overflow time.
    uint64_t current_bytes;               ///< Queued bytes at overflow time.
    std::function<bool()> drop_oldest_fn; ///< Callback to evict the oldest user
                                          ///< message.
    mailbox::DeadLetterQueue* dlq = nullptr; ///< Optional DLQ pointer (may be
                                             ///< null).
    std::function<bool()> drop_lowest_priority_fn; ///< Callback to evict the
                                                   ///< lowest-priority user
                                                   ///< message.
};

} // namespace hpactor::mailbox::detail
