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

#include <hpactor/mailbox/dedup_cache.hpp>

#include <mutex>
#include <unordered_map>

namespace hpactor::mailbox {

// ── Key type ──────────────────────────────────────────────────────────────

namespace {

struct DedupKey {
    EndPoint source_node;
    ActorId source_actor;
    uint64_t message_id;

    bool operator==(const DedupKey& other) const noexcept {
        return source_node == other.source_node &&
               source_actor == other.source_actor &&
               message_id == other.message_id;
    }
};

struct DedupKeyHash {
    size_t operator()(const DedupKey& k) const noexcept {
        size_t h = std::hash<EndPoint>{}(k.source_node);
        h = h * 31 + static_cast<size_t>(k.source_actor.value());
        h = h * 31 + static_cast<size_t>(k.message_id);
        return h;
    }
};

} // anonymous namespace

// ── Impl ──────────────────────────────────────────────────────────────────

struct DedupCache::Impl {
    Config cfg;
    std::mutex mutex;
    std::unordered_map<DedupKey, uint64_t, DedupKeyHash> entries;
    uint64_t hits{0};
    uint64_t inserts{0};
};

// ── Public API ────────────────────────────────────────────────────────────

DedupCache::DedupCache(Config cfg) : impl_(new Impl) {
    impl_->cfg = std::move(cfg);
}

DedupCache::~DedupCache() = default;

DedupCache::DedupCache(DedupCache&&) noexcept = default;
DedupCache& DedupCache::operator=(DedupCache&&) noexcept = default;

bool DedupCache::is_duplicate(EndPoint source_node, ActorId source_actor,
                               MessageId message_id) noexcept {
    DedupKey key{source_node, source_actor, message_id.value()};

    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->entries.find(key);
    if (it != impl_->entries.end()) {
        impl_->hits++;
        return true;
    }

    // Not found — insert with a zero timestamp.
    // Real timestamps are assigned in purge_expired() or can be added
    // later. Entries with timestamp 0 are treated as unexpired until
    // assigned a real timestamp.
    impl_->entries[key] = 0;
    impl_->inserts++;

    // Evict oldest entries if over capacity.
    if (impl_->entries.size() > impl_->cfg.max_entries) {
        size_t to_remove = impl_->cfg.max_entries / 10;
        if (to_remove == 0)
            to_remove = 1;
        auto it2 = impl_->entries.begin();
        for (size_t i = 0; i < to_remove && it2 != impl_->entries.end(); ++i) {
            it2 = impl_->entries.erase(it2);
        }
    }

    return false;
}

void DedupCache::purge_expired(uint64_t now_ns) noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);

    auto it = impl_->entries.begin();
    while (it != impl_->entries.end()) {
        uint64_t entry_ts = it->second;
        // Entries with timestamp 0 were inserted by is_duplicate() and
        // haven't been timestamped yet — assign now_ns so they become
        // eligible for expiry on the next purge cycle.
        if (entry_ts == 0) {
            it->second = now_ns;
            ++it;
            continue;
        }
        if ((now_ns - entry_ts) > impl_->cfg.ttl_ns) {
            it = impl_->entries.erase(it);
        } else {
            ++it;
        }
    }
}

size_t DedupCache::size() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->entries.size();
}

uint64_t DedupCache::duplicate_hits() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->hits;
}

uint64_t DedupCache::insertions() const noexcept {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->inserts;
}

} // namespace hpactor::mailbox
