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

#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/msg/outbound_delivery_tracker.hpp>

namespace hpactor::msg {

OutboundDeliveryTracker::OutboundDeliveryTracker() = default;
OutboundDeliveryTracker::~OutboundDeliveryTracker() = default;

DeliveryReceipt
OutboundDeliveryTracker::track(StreamBuffer serialized_frame, EndPoint remote,
                               RetryPolicy policy, uint64_t deadline_ns) {
    std::lock_guard<std::mutex> lk(mutex_);

    uint64_t raw_id = next_msg_id_.fetch_add(1, std::memory_order_relaxed);
    MessageId msg_id{raw_id};

    auto state = std::make_shared<DeliveryReceipt::SharedState>();
    state->msg_id = msg_id;

    PendingSend ps;
    ps.msg_id = msg_id;
    ps.serialized_frame = std::move(serialized_frame);
    ps.remote_endpoint = remote;
    ps.policy = policy;
    ps.deadline_ns = deadline_ns;
    ps.receipt = DeliveryReceipt(state);

    pending_.emplace(raw_id, std::move(ps));

    emit_metric(::hpactor::metrics::MetricEventType::kReliableTracked, 0);
    return DeliveryReceipt(state);
}

void OutboundDeliveryTracker::on_ack(MessageId msg_id, EndPoint /*from*/) {
    mailbox::DeliveryResult result;
    result.status = mailbox::DeliveryStatus::Accepted;
    result.message_id = msg_id;
    resolve(msg_id, result);
    emit_metric(::hpactor::metrics::MetricEventType::kReliableAckReceived,
                static_cast<uint8_t>(mailbox::DeliveryStatus::Accepted));
}

static bool is_retryable_nack(mailbox::DeliveryStatus s) {
    return s == mailbox::DeliveryStatus::MailboxFull;
}

void OutboundDeliveryTracker::on_nack(MessageId msg_id, EndPoint /*from*/,
                                      uint32_t reason_code,
                                      uint32_t retry_after_ms) {
    auto status = static_cast<mailbox::DeliveryStatus>(reason_code);

    // Duplicate is treated as ACK — receiver already has the message.
    if (status == mailbox::DeliveryStatus::Duplicate) {
        on_ack(msg_id, EndPoint{});
        return;
    }

    std::unique_lock<std::mutex> lk(mutex_);
    auto it = pending_.find(msg_id.value());
    if (it == pending_.end())
        return;

    emit_metric(::hpactor::metrics::MetricEventType::kReliableNackReceived,
                static_cast<uint8_t>(status));

    if (is_retryable_nack(status)) {
        // Schedule retry; honour the receiver's retry-after hint.
        it->second.retry_after_ms = retry_after_ms;
        it->second.next_retry_ns = 1; // sentinel for "ASAP"
    } else {
        // Non-retryable: resolve immediately.
        mailbox::DeliveryResult result;
        result.status = status;
        result.message_id = msg_id;
        auto receipt = std::move(it->second.receipt);
        pending_.erase(it);
        lk.unlock();
        if (auto st = receipt.state_) {
            st->resolve(result);
        }
    }
}

void OutboundDeliveryTracker::process_retries(
    uint64_t now_ns, std::function<void(const PendingSend&)> resend_callback) {
    // Store (result, SharedState) pairs so we can resolve outside the lock
    // without re-looking-up entries that were already erased.
    std::vector<std::pair<mailbox::DeliveryResult, std::shared_ptr<DeliveryReceipt::SharedState>>>
        to_resolve;

    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.begin();
        while (it != pending_.end()) {
            auto& ps = it->second;
            bool expired = ps.deadline_ns > 0 && now_ns >= ps.deadline_ns;

            if (ps.next_retry_ns > 0 && now_ns >= ps.next_retry_ns) {
                // Pre-increment check: will the NEXT attempt exceed the limit?
                if (ps.retry_count + 1 >= ps.policy.max_attempts || expired) {
                    mailbox::DeliveryResult exhausted;
                    exhausted.status =
                        expired ? mailbox::DeliveryStatus::Expired
                                : mailbox::DeliveryStatus::TransportError;
                    exhausted.message_id = ps.msg_id;
                    uint8_t count = ps.retry_count;
                    to_resolve.emplace_back(exhausted, ps.receipt.state_);
                    emit_metric(::hpactor::metrics::MetricEventType::kReliableExhausted,
                                count);
                    it = pending_.erase(it);
                    continue;
                }
                ps.retry_count++;
                // Use the hint from NackFrame if present, otherwise backoff.
                uint64_t delay_ns;
                if (ps.retry_after_ms > 0) {
                    delay_ns =
                        static_cast<uint64_t>(ps.retry_after_ms) * 1'000'000ULL;
                } else {
                    auto backoff = ps.policy.backoff_delay(ps.retry_count);
                    delay_ns = static_cast<uint64_t>(backoff.count()) * 1'000'000ULL;
                }
                ps.next_retry_ns = now_ns + delay_ns;
                uint8_t count = ps.retry_count;
                emit_metric(::hpactor::metrics::MetricEventType::kReliableRetry,
                            count);
                // Call resend callback directly inside the lock.
                // transport->try_send() is non-blocking edge-triggered I/O
                // — no mutex acquisition, safe to call under our lock.
                resend_callback(ps);
            } else if (ps.next_retry_ns == 0 &&
                       ps.policy.per_attempt_timeout.count() > 0) {
                // First send: start the per-attempt timeout.
                if (expired) {
                    mailbox::DeliveryResult expired_result;
                    expired_result.status = mailbox::DeliveryStatus::Expired;
                    expired_result.message_id = ps.msg_id;
                    to_resolve.emplace_back(expired_result, ps.receipt.state_);
                    emit_metric(::hpactor::metrics::MetricEventType::kReliableExhausted,
                                ps.retry_count);
                    it = pending_.erase(it);
                    continue;
                }
                ps.next_retry_ns =
                    now_ns +
                    static_cast<uint64_t>(ps.policy.per_attempt_timeout.count()) *
                        1'000'000ULL;
            }
            ++it;
        }
    }

    for (auto& [result, state] : to_resolve) {
        if (state) {
            state->resolve(result);
        }
    }
}

void OutboundDeliveryTracker::cancel_endpoint(EndPoint endpoint,
                                              mailbox::DeliveryStatus reason) {
    std::vector<std::pair<mailbox::DeliveryResult, std::shared_ptr<DeliveryReceipt::SharedState>>>
        to_resolve;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.begin();
        while (it != pending_.end()) {
            if (it->second.remote_endpoint == endpoint) {
                mailbox::DeliveryResult result;
                result.status = reason;
                result.message_id = it->second.msg_id;
                to_resolve.emplace_back(result, it->second.receipt.state_);
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }
    size_t cancelled_count = to_resolve.size();
    for (auto& [result, state] : to_resolve) {
        if (state) {
            state->resolve(result);
        }
    }
    if (cancelled_count > 0) {
        emit_metric(
            ::hpactor::metrics::MetricEventType::kReliableCancelled,
            static_cast<uint8_t>(cancelled_count > 255 ? 255 : cancelled_count));
    }
}

void OutboundDeliveryTracker::cancel(MessageId msg_id) {
    mailbox::DeliveryResult result;
    result.status = mailbox::DeliveryStatus::Cancelled;
    result.message_id = msg_id;
    resolve(msg_id, result);
    emit_metric(::hpactor::metrics::MetricEventType::kReliableCancelled, 0);
}

size_t OutboundDeliveryTracker::pending() const noexcept {
    std::lock_guard<std::mutex> lk(mutex_);
    return pending_.size();
}

std::vector<OutboundDeliveryTracker::SnapshotEntry>
OutboundDeliveryTracker::snapshot() const {
    std::lock_guard<std::mutex> lk(mutex_);
    std::vector<SnapshotEntry> result;
    result.reserve(pending_.size());
    for (const auto& [id, ps] : pending_) {
        result.push_back(
            {ps.msg_id, ps.retry_count, ps.deadline_ns, ps.next_retry_ns});
    }
    return result;
}

void OutboundDeliveryTracker::resolve(MessageId msg_id,
                                      mailbox::DeliveryResult result) {
    std::shared_ptr<DeliveryReceipt::SharedState> state;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        auto it = pending_.find(msg_id.value());
        if (it != pending_.end()) {
            state = it->second.receipt.state_;
            pending_.erase(it);
        }
    }
    if (state) {
        state->resolve(result);
    }
}

void OutboundDeliveryTracker::emit_metric(::hpactor::metrics::MetricEventType type,
                                          uint8_t code) {
    if (metrics_cb_) {
        uint64_t now_ns = 0; // Caller can provide timestamp if needed.
        metrics_cb_(now_ns, type, code);
    }
}

} // namespace hpactor::msg
