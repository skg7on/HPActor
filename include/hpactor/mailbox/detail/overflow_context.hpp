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
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/mailbox/overflow_queue.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>

#include <atomic>
#include <functional>

namespace hpactor::mailbox {
class DeadLetterQueue;
}

namespace hpactor::mailbox::detail {

template <typename T> struct OverflowContext {
    const T& message;
    MailboxEnvelopeMeta& meta;
    ReservationManager<T>& reservation;
    OverflowQueue<T>& overflow_queue;
    std::atomic<uint64_t>& total_rejected;
    std::atomic<uint64_t>& total_dropped;
    std::atomic<uint64_t>& total_dead_letters;
    metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_buf;
    MailboxConfig& config;
    ActorId actor_id;
    uint32_t current_depth;
    uint64_t current_bytes;
    std::function<bool()> drop_oldest_fn;
    mailbox::DeadLetterQueue* dlq = nullptr;
};

} // namespace hpactor::mailbox::detail
