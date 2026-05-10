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

#include <hpactor/mailbox/dead_letter_queue.hpp>

namespace hpactor::mailbox {

DeadLetterQueue::DeadLetterQueue(DeadLetterConfig config) : config_(config) {
    if (config_.capacity == 0) {
        config_.capacity = 4096;
    }
}

void DeadLetterQueue::trim_payload(DeadLetterRecord& record) const {
    record.payload_size = static_cast<uint32_t>(record.payload_sample.size());
    if (!config_.store_payload) {
        record.payload_sample.clear();
        return;
    }
    if (record.payload_sample.size() > config_.max_payload_sample_bytes) {
        record.payload_sample.resize(config_.max_payload_sample_bytes);
    }
}

bool DeadLetterQueue::try_push(DeadLetterRecord&& record) noexcept {
    if (!config_.enabled) {
        return false;
    }

    trim_payload(record);

    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.size() >= config_.capacity) {
        switch (config_.overflow_policy) {
            case DeadLetterOverflowPolicy::DropOldestRecord:
                records_.pop_front();
                total_lost_++;
                break;
            case DeadLetterOverflowPolicy::DropNewestRecord:
                total_lost_++;
                return false;
            case DeadLetterOverflowPolicy::MetadataOnly:
                record.payload_sample.clear();
                if (records_.size() >= config_.capacity) {
                    records_.pop_front();
                    total_lost_++;
                }
                break;
        }
    }

    records_.push_back(std::move(record));
    total_pushed_++;
    return true;
}

bool DeadLetterQueue::try_pop(DeadLetterRecord& out) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (records_.empty()) {
        return false;
    }
    out = std::move(records_.front());
    records_.pop_front();
    total_popped_++;
    return true;
}

DeadLetterQueueSnapshot DeadLetterQueue::snapshot() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    DeadLetterQueueSnapshot s;
    s.depth = static_cast<uint32_t>(records_.size());
    s.capacity = config_.capacity;
    s.total_pushed = total_pushed_;
    s.total_popped = total_popped_;
    s.total_lost = total_lost_;
    return s;
}

} // namespace hpactor::mailbox
