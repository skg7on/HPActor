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

#include <hpactor/actor/routing/routing_logic.hpp>

#include <algorithm>
#include <cstdint>

namespace hpactor::routing {

// ── RoundRobinLogic ────────────────────────────────────────────────────────

size_t RoundRobinLogic::select_routee(
    const std::vector<ActorRef>& routees, const TypedMessage& /*msg*/,
    const std::vector<cli::MboxSnapshot>& /*routee_states*/) {
    if (routees.empty()) {
        return 0;
    }
    uint64_t count = counter_.fetch_add(1, std::memory_order_relaxed);
    return static_cast<size_t>(count % routees.size());
}

// ── RandomLogic ────────────────────────────────────────────────────────────

RandomLogic::RandomLogic(uint64_t seed) {
    uint64_t s = seed;
    if (s == 0) {
        // Non-deterministic fallback: hash of our own address, mixed.
        s = reinterpret_cast<uint64_t>(this);
        s ^= s >> 30;
        s *= 0xbf58476d1ce4e5b9ULL;
        s ^= s >> 27;
        s ^= s << 13;
    }
    state_.store(s, std::memory_order_relaxed);
}

size_t
RandomLogic::select_routee(const std::vector<ActorRef>& routees,
                           const TypedMessage& /*msg*/,
                           const std::vector<cli::MboxSnapshot>& /*routee_states*/) {
    if (routees.empty()) {
        return 0;
    }

    // xorshift64 — read current, compute next, try to CAS.
    uint64_t x = state_.load(std::memory_order_relaxed);
    uint64_t next = x;
    next ^= next << 13;
    next ^= next >> 7;
    next ^= next << 17;
    // If the CAS fails, x is reloaded with the real current value.
    // Either way we return the value that was "current" when we started.
    state_.compare_exchange_weak(x, next, std::memory_order_relaxed,
                                 std::memory_order_relaxed);
    (void)next;

    return static_cast<size_t>(x % routees.size());
}

// ── ConsistentHashingLogic ────────────────────────────────────────────────

static uint64_t default_key_extractor(const TypedMessage& msg) {
    return static_cast<uint64_t>(msg.type_id());
}

ConsistentHashingLogic::ConsistentHashingLogic(uint32_t virtual_nodes_per_routee,
                                               KeyExtractor key_extractor)
    : vnodes_per_routee_(virtual_nodes_per_routee),
      key_extractor_(key_extractor ? std::move(key_extractor)
                                   : default_key_extractor) {}

void ConsistentHashingLogic::rebuild_ring(const std::vector<ActorRef>& routees) {
    ring_.clear();
    if (routees.empty()) {
        return;
    }

    ring_.reserve(routees.size() * vnodes_per_routee_);

    for (size_t i = 0; i < routees.size(); ++i) {
        // Hash the routee's ActorId as the base for virtual nodes.
        uint64_t base_hash = std::hash<ActorId>{}(routees[i].address().id);

        for (uint32_t v = 0; v < vnodes_per_routee_; ++v) {
            // Combine base hash with virtual node index using splitmix64
            // steps to spread the ring.
            uint64_t vnode_hash = base_hash;
            vnode_hash ^= vnode_hash >> 30;
            vnode_hash *= 0xbf58476d1ce4e5b9ULL;
            vnode_hash ^= vnode_hash >> 27;
            vnode_hash += static_cast<uint64_t>(v) * 0x94d049bb133111ebULL;
            ring_.emplace_back(vnode_hash, i);
        }
    }

    std::sort(ring_.begin(), ring_.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
}

size_t ConsistentHashingLogic::select_routee(
    const std::vector<ActorRef>& routees, const TypedMessage& msg,
    const std::vector<cli::MboxSnapshot>& /*routee_states*/) {
    if (routees.empty() || ring_.empty()) {
        return 0;
    }

    uint64_t key = key_extractor_(msg);

    // Find the first virtual node with hash >= key (wrap to ring[0] if past
    // the end).
    auto it = std::lower_bound(ring_.begin(), ring_.end(), key,
                               [](const std::pair<uint64_t, size_t>& entry,
                                  uint64_t k) { return entry.first < k; });

    if (it == ring_.end()) {
        it = ring_.begin();
    }

    return it->second;
}

// ── SmallestMailboxLogic ──────────────────────────────────────────────────

size_t SmallestMailboxLogic::select_routee(
    const std::vector<ActorRef>& routees, const TypedMessage& /*msg*/,
    const std::vector<cli::MboxSnapshot>& routee_states) {
    if (routees.empty()) {
        return 0;
    }

    size_t best = 0;
    uint32_t min_depth = UINT32_MAX;

    for (size_t i = 0; i < routees.size(); ++i) {
        uint32_t depth = (i < routee_states.size()) ? routee_states[i].depth
                                                    : 0; // unknown → treat as
                                                         // empty
        if (depth < min_depth) {
            min_depth = depth;
            best = i;
        }
    }

    return best;
}

} // namespace hpactor::routing
