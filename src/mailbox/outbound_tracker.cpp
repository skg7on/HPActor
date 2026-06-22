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

#include <hpactor/mailbox/outbound_tracker.hpp>

#include <algorithm>
#include <mutex>

namespace hpactor::mailbox {

OutboundTracker::OutboundTracker(ReliableRetryPolicy policy)
    : policy_(policy) {}

bool OutboundTracker::track(MessageId msg_id, ActorAddress target,
                            StreamBuffer payload,
                            MonotonicClock::time_point deadline) {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t dest_count = 0;
    for (const auto& [id, entry] : entries_) {
        (void)id;
        if (entry.target.node_id() == target.node_id())
            ++dest_count;
    }
    if (dest_count >= kMaxPendingPerDestination)
        return false;
    auto now = MonotonicClock::now();
    entries_[msg_id.value()] = OutboundTrackerEntry{
        msg_id, target, std::move(payload), 0, now, deadline};
    return true;
}

void OutboundTracker::on_ack(MessageId msg_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(msg_id.value());
}

void OutboundTracker::on_nack(MessageId msg_id, Duration retry_after) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = entries_.find(msg_id.value());
    if (it == entries_.end())
        return;
    if (!policy_.should_retry(it->second.retry_count)) {
        expired_.push_back(std::move(it->second));
        entries_.erase(it);
        return;
    }
    it->second.retry_count++;
    it->second.next_retry_at = MonotonicClock::now() + retry_after;
}

void OutboundTracker::tick(MonotonicClock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        auto& entry = it->second;
        if (entry.deadline != MonotonicClock::time_point::max() &&
            now >= entry.deadline) {
            expired_.push_back(std::move(entry));
            it = entries_.erase(it);
            continue;
        }
        if (now >= entry.next_retry_at &&
            entry.next_retry_at != MonotonicClock::time_point::min()) {
            if (policy_.should_retry(entry.retry_count)) {
                entry.retry_count++;
                auto backoff = policy_.backoff_for_attempt(entry.retry_count);
                entry.next_retry_at = now + backoff;
            } else {
                expired_.push_back(std::move(entry));
                it = entries_.erase(it);
                continue;
            }
        }
        ++it;
    }
}

void OutboundTracker::fail_pending_for_node(const std::string& node_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->second.target.node_id() == node_id) {
            expired_.push_back(std::move(it->second));
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t OutboundTracker::pending_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

std::vector<OutboundTrackerEntry> OutboundTracker::drain_expired() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<OutboundTrackerEntry> drained;
    drained.swap(expired_);
    return drained;
}

} // namespace hpactor::mailbox
