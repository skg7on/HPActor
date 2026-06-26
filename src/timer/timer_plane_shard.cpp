// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/timer/timer_plane_shard.hpp>

#include <algorithm>
#include <climits>

namespace hpactor::sched {

// ============================================================================
// BucketList
// ============================================================================

void TimerPlaneShard::BucketList::push_back(TimerNode* node) {
    node->next = nullptr;
    node->prev = tail;
    if (tail) {
        tail->next = node;
    } else {
        head = node;
    }
    tail = node;
    count++;
}

void TimerPlaneShard::BucketList::unlink(TimerNode* node) {
    if (node->prev) {
        node->prev->next = node->next;
    } else {
        head = node->next;
    }
    if (node->next) {
        node->next->prev = node->prev;
    } else {
        tail = node->prev;
    }
    node->next = nullptr;
    node->prev = nullptr;
    count--;
}

// ============================================================================
// Constructor / Destructor
// ============================================================================

TimerPlaneShard::TimerPlaneShard(uint32_t shard_index, int64_t tick_ns)
    : shard_index_(shard_index), tick_ns_(tick_ns) {
    // Pre-allocate slot array.
    slots_.resize(kMaxSlots, nullptr);
    generations_.resize(kMaxSlots, 0);
    free_slots_.reserve(kMaxSlots);
    // Fill free list in reverse so first acquire gets slot 0.
    for (uint32_t i = kMaxSlots; i > 0; --i) {
        free_slots_.push_back(i - 1);
    }

    // Initialize wheel levels.
    levels_.resize(kNumLevels);
    level_ranges_.resize(kNumLevels);

    for (uint32_t level = 0; level < kNumLevels; ++level) {
        levels_[level].buckets.resize(kBucketsPerLevel);
        levels_[level].mask = kBucketsPerLevel - 1;

        // Precompute range for this level: tick_ns * 256^(level+1)
        int64_t range = tick_ns_;
        for (uint32_t k = 0; k <= level; ++k) {
            range *= kBucketsPerLevel;
        }
        level_ranges_[level] = range;
    }
}

TimerPlaneShard::~TimerPlaneShard() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Drain the command queue to free any TimerNodes in pending Schedule
    // commands.
    {
        std::vector<TimerCommand> cmds;
        cmd_queue_.drain_all(cmds);
        for (auto& cmd : cmds) {
            if (cmd.type == TimerCommand::Type::Schedule && cmd.schedule.node) {
                delete cmd.schedule.node;
            }
        }
    }
    for (auto& level : levels_) {
        for (auto& bucket : level.buckets) {
            TimerNode* t = bucket.head;
            while (t) {
                TimerNode* next = t->next;
                delete t;
                t = next;
            }
            bucket.head = nullptr;
            bucket.tail = nullptr;
            bucket.count = 0;
        }
    }
}

// ============================================================================
// Slot management
// ============================================================================

uint32_t TimerPlaneShard::acquire_slot(TimerNode* node) {
    if (free_slots_.empty()) {
        return kInvalidSlot; // No free slots
    }

    uint32_t slot = free_slots_.back();
    free_slots_.pop_back();

    // Increment generation, skip 0 to avoid ABA with uninitialized slots.
    uint8_t gen = ++generations_[slot];
    if (gen == 0) {
        gen = ++generations_[slot]; // Wrap past 0
    }

    slots_[slot] = node;
    node->slot_index = slot;
    node->generation = gen;

    return slot;
}

void TimerPlaneShard::release_slot(uint32_t slot_index, uint8_t expected_generation) {
    if (slot_index >= kMaxSlots)
        return;
    if (generations_[slot_index] != expected_generation)
        return; // ABA guard

    slots_[slot_index] = nullptr;
    free_slots_.push_back(slot_index);
}

// ============================================================================
// Wheel insertion / removal
// ============================================================================

void TimerPlaneShard::insert_into_wheel(TimerNode* node) {
    int64_t expire = node->expire_ns;
    int64_t now = current_time_ns_;

    // Clamp to at least one tick in the future.
    expire = std::max(expire, now + tick_ns_);

    int64_t diff = expire - now;
    uint32_t level = kNumLevels - 1; // Default to highest level.

    for (uint32_t l = 0; l < kNumLevels; ++l) {
        if (diff < level_ranges_[l]) {
            level = l;
            break;
        }
    }

    // Compute bucket from absolute expiry (not current time) so the timer
    // lands in the correct slot of the granularity wheel.
    int64_t bucket_key = expire / tick_ns_;
    for (uint32_t l = 0; l < level; ++l) {
        bucket_key /= kBucketsPerLevel;
    }
    uint32_t bucket = static_cast<uint32_t>(bucket_key) & levels_[level].mask;

    node->wheel_level = static_cast<uint8_t>(level);
    node->bucket_idx = bucket;
    levels_[level].buckets[bucket].push_back(node);
}

void TimerPlaneShard::remove_from_wheel(TimerNode* node) {
    auto& bucket = levels_[node->wheel_level].buckets[node->bucket_idx];
    bucket.unlink(node);
}

// ============================================================================
// Public API
// ============================================================================

TimerHandle TimerPlaneShard::schedule(int64_t delay_ns, timer_callback cb,
                                      uint64_t group_handle, uint8_t priority) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto* node = new TimerNode;
    node->expire_ns = current_time_ns_ + delay_ns;
    node->callback = std::move(cb);
    node->group_handle = group_handle;
    node->priority = priority;

    uint32_t slot = acquire_slot(node);
    if (slot == kInvalidSlot) {
        delete node;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return TimerHandle{}; // Invalid handle.
    }

    insert_into_wheel(node);

    // Clamp node->expire_ns for accurate min_deadline_ tracking
    // (insert_into_wheel already clamps for wheel placement).
    node->expire_ns = std::max(node->expire_ns, current_time_ns_ + tick_ns_);

    // Update cached minimum deadline.
    int64_t cur = min_deadline_.load(std::memory_order_relaxed);
    if (node->expire_ns < cur) {
        min_deadline_.store(node->expire_ns, std::memory_order_release);
    }

    pending_.fetch_add(1, std::memory_order_relaxed);
    scheduled_.fetch_add(1, std::memory_order_relaxed);

    return TimerHandle::make_encoded(shard_index_, slot, node->generation, 0);
}

bool TimerPlaneShard::cancel(TimerHandle handle) {
    std::lock_guard<std::mutex> lock(mutex_);

    uint32_t shard = TimerHandle::shard_index(handle);
    if (shard != shard_index_)
        return false;

    uint32_t slot = TimerHandle::slot_index(handle);
    uint8_t gen = TimerHandle::generation(handle);

    if (slot >= kMaxSlots)
        return false;

    TimerNode* node = slots_[slot];
    if (!node)
        return false;
    if (generations_[slot] != gen)
        return false; // ABA guard.

    // O(1) unlink from wheel.
    remove_from_wheel(node);

    // Clear callback to release any captured resources.
    node->callback = {};

    int64_t expired = node->expire_ns;

    release_slot(slot, gen);

    // If the cancelled timer was the earliest, recompute.
    if (expired <= min_deadline_.load(std::memory_order_relaxed)) {
        recompute_min_deadline();
    }

    delete node;

    pending_.fetch_sub(1, std::memory_order_relaxed);
    cancelled_.fetch_add(1, std::memory_order_relaxed);

    return true;
}

uint32_t TimerPlaneShard::advance(int64_t now_ns) {
    // Collect expired timer callbacks under the lock, then fire them
    // outside the lock (same pattern as TimingWheel::advance).
    std::vector<timer_callback> pending;

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // ---- Drain cross-thread command queue first ----
        bool drained_commands = false;
        {
            std::vector<TimerCommand> cmds;
            cmd_queue_.drain_all(cmds);
            drained_commands = !cmds.empty();
            for (auto& cmd : cmds) {
                switch (cmd.type) {
                    case TimerCommand::Type::Schedule: {
                        auto* node = cmd.schedule.node;
                        node->expire_ns = cmd.schedule.expire_ns;

                        uint32_t slot = acquire_slot(node);
                        if (slot == kInvalidSlot) {
                            delete node;
                            dropped_.fetch_add(1, std::memory_order_relaxed);
                            continue;
                        }
                        insert_into_wheel(node);

                        int64_t cur = min_deadline_.load(std::memory_order_relaxed);
                        if (node->expire_ns < cur) {
                            min_deadline_.store(node->expire_ns,
                                                std::memory_order_release);
                        }
                        pending_.fetch_add(1, std::memory_order_relaxed);
                        scheduled_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    case TimerCommand::Type::Cancel: {
                        uint64_t h = cmd.cancel.handle;
                        TimerHandle th{h};

                        uint32_t c_shard = TimerHandle::shard_index(th);
                        if (c_shard != shard_index_)
                            continue;

                        uint32_t c_slot = TimerHandle::slot_index(th);
                        uint8_t c_gen = TimerHandle::generation(th);

                        if (c_slot >= kMaxSlots)
                            continue;

                        TimerNode* c_node = slots_[c_slot];
                        if (!c_node)
                            continue;
                        if (generations_[c_slot] != c_gen)
                            continue;

                        remove_from_wheel(c_node);
                        c_node->callback = {};
                        int64_t c_expired = c_node->expire_ns;
                        release_slot(c_slot, c_gen);

                        if (c_expired <=
                            min_deadline_.load(std::memory_order_relaxed)) {
                            recompute_min_deadline();
                        }

                        delete c_node;
                        pending_.fetch_sub(1, std::memory_order_relaxed);
                        cancelled_.fetch_add(1, std::memory_order_relaxed);
                        break;
                    }
                    case TimerCommand::Type::DrainGroup: {
                        uint64_t gh = cmd.drain_group.group_handle;
                        // Walk all buckets and cancel timers belonging to the
                        // group.
                        for (auto& level : levels_) {
                            for (auto& bucket : level.buckets) {
                                TimerNode* t = bucket.head;
                                while (t) {
                                    TimerNode* next = t->next;
                                    if (t->group_handle == gh) {
                                        bucket.unlink(t);
                                        t->callback = {};
                                        release_slot(t->slot_index, t->generation);
                                        delete t;
                                        pending_.fetch_sub(
                                            1, std::memory_order_relaxed);
                                        cancelled_.fetch_add(
                                            1, std::memory_order_relaxed);
                                    }
                                    t = next;
                                }
                            }
                        }
                        recompute_min_deadline();
                        break;
                    }
                }
            }
        }

        if (now_ns <= current_time_ns_) {
            if (drained_commands) {
                recompute_min_deadline();
            }
            return 0;
        }

        // Cap the advance step to prevent a positive feedback loop (same
        // rationale as TimingWheel::advance).
        static constexpr int64_t kMaxAdvanceNs = 100'000'000; // 100 ms
        if (now_ns - current_time_ns_ > kMaxAdvanceNs) {
            now_ns = current_time_ns_ + kMaxAdvanceNs;
        }

        int64_t old_time = current_time_ns_;
        current_time_ns_ = now_ns;

        // Process all levels.
        for (uint32_t level = 0; level < kNumLevels; ++level) {
            int64_t old_key = old_time / tick_ns_;
            for (uint32_t l = 0; l < level; ++l) {
                old_key /= kBucketsPerLevel;
            }
            uint32_t start_bucket =
                static_cast<uint32_t>(old_key) & levels_[level].mask;

            int64_t new_key = now_ns / tick_ns_;
            for (uint32_t l = 0; l < level; ++l) {
                new_key /= kBucketsPerLevel;
            }
            uint32_t end_bucket =
                static_cast<uint32_t>(new_key) & levels_[level].mask;

            uint32_t count = ((end_bucket - start_bucket + kBucketsPerLevel) %
                              kBucketsPerLevel) +
                             1;

            for (uint32_t i = 0; i < count; ++i) {
                uint32_t bucket_idx = (start_bucket + i) % kBucketsPerLevel;
                auto& bucket = levels_[level].buckets[bucket_idx];

                TimerNode* t = bucket.head;
                while (t) {
                    TimerNode* next = t->next;

                    int64_t timer_expire = t->expire_ns;
                    if (timer_expire <= now_ns) {
                        // Fire this timer.
                        bucket.unlink(t);
                        pending.push_back(std::move(t->callback));
                        release_slot(t->slot_index, t->generation);
                        delete t;
                        pending_.fetch_sub(1, std::memory_order_relaxed);
                        fired_.fetch_add(1, std::memory_order_relaxed);

                        // Late metric: fired after expiry by more than
                        // tick_ns_.
                        if (now_ns - timer_expire > tick_ns_) {
                            late_.fetch_add(1, std::memory_order_relaxed);
                        }
                    } else if (level > 0) {
                        // Cascade to the next lower level.  Compute target
                        // bucket from absolute expiry so the timer lands at
                        // the correct position.
                        uint32_t lower = level - 1;
                        int64_t bk = t->expire_ns / tick_ns_;
                        for (uint32_t l = 0; l < lower; ++l) {
                            bk /= kBucketsPerLevel;
                        }
                        uint32_t lb =
                            static_cast<uint32_t>(bk) & levels_[lower].mask;

                        bucket.unlink(t);
                        t->wheel_level = static_cast<uint8_t>(lower);
                        t->bucket_idx = lb;
                        levels_[lower].buckets[lb].push_back(t);
                    }
                    // else level == 0 and not expired: leave in place.

                    t = next;
                }
            }
        }

        if (!pending.empty()) {
            recompute_min_deadline();
        }
    }
    // Lock released -- fire callbacks safely outside the critical section.

    for (auto& cb : pending) {
        cb();
    }

    return static_cast<uint32_t>(pending.size());
}

bool TimerPlaneShard::push_command(TimerCommand cmd) {
    return cmd_queue_.try_push(cmd);
}

// ============================================================================
// Private helpers
// ============================================================================

void TimerPlaneShard::recompute_min_deadline() {
    int64_t m = INT64_MAX;
    for (const auto& level : levels_) {
        for (const auto& bucket : level.buckets) {
            for (TimerNode* n = bucket.head; n; n = n->next) {
                m = std::min(m, n->expire_ns);
            }
        }
    }
    min_deadline_.store(m, std::memory_order_release);
}

} // namespace hpactor::sched
