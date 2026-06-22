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

#include <hpactor/mailbox/in_memory_delivery_store.hpp>

#include <chrono>
#include <mutex>

namespace hpactor::mailbox {

using msg::PendingSend;

result<void> InMemoryDeliveryStore::put_outbox(const PendingSend& record) {
    std::lock_guard<std::mutex> lock(mutex_);
    outbox_[record.message_id.value()] = record;
    return result<void>::make();
}

result<void> InMemoryDeliveryStore::mark_outbox_complete(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    outbox_.erase(id.value());
    return result<void>::make();
}

result<std::vector<PendingSend>> InMemoryDeliveryStore::load_pending_outbox() {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PendingSend> items;
    for (const auto& [id, send] : outbox_) {
        (void)id;
        items.push_back(send);
    }
    return result<std::vector<PendingSend>>::make(std::move(items));
}

result<void> InMemoryDeliveryStore::put_inbox(MessageId id, uint64_t ttl_ns) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    inbox_[id.value()] = static_cast<uint64_t>(now) + ttl_ns;
    return result<void>::make();
}

result<bool> InMemoryDeliveryStore::seen_inbox(MessageId id) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = inbox_.find(id.value());
    if (it == inbox_.end()) {
        return result<bool>::make(false);
    }
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    if (static_cast<uint64_t>(now) > it->second) {
        inbox_.erase(it);
        return result<bool>::make(false);
    }
    return result<bool>::make(true);
}

} // namespace hpactor::mailbox
