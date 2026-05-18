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

#include <hpactor/sched/timing_wheel.hpp>

#include <chrono>

namespace hpactor::sched {

TimingWheel::TimingWheel(int64_t tick_ns, uint32_t num_levels)
    : tick_ns_(tick_ns), num_levels_(num_levels), levels_(num_levels),
      current_time_(std::chrono::steady_clock::now().time_since_epoch().count()) {
    // Initialize each level
    // Level 0: 256 buckets of tick_ns each
    // Level 1: 256 buckets of 256 * tick_ns each
    // etc.
    for (uint32_t level = 0; level < num_levels; ++level) {
        uint32_t num_buckets = 256;

        levels_[level].buckets.resize(num_buckets);
        levels_[level].num_buckets = num_buckets;
        levels_[level].mask = num_buckets - 1;
    }
}

TimingWheel::~TimingWheel() {
    // Cancel all timers
    for (Timer* timer : all_timers_) {
        timer->cancelled = true;
        delete timer;
    }
}

uint64_t TimingWheel::schedule(int64_t delay_ns, TimerCallback callback) {
    return schedule_at(current_time_.load(std::memory_order_relaxed) + delay_ns,
                       std::move(callback));
}

uint64_t TimingWheel::schedule_at(int64_t expire_ns, TimerCallback callback) {
    return add_timer_internal(expire_ns, std::move(callback));
}

uint64_t
TimingWheel::add_timer_internal(int64_t expire_ns, TimerCallback callback) {
    auto* timer = new Timer;
    timer->expire_ns = expire_ns;
    timer->id = next_timer_id_.fetch_add(1);
    timer->callback = std::move(callback);
    timer->cancelled = false;

    insert_timer(timer);
    return timer->id;
}

bool TimingWheel::cancel(uint64_t timer_id) {
    Timer* timer = remove_timer(timer_id);
    if (timer) {
        timer->cancelled = true;
        return true;
    }
    return false;
}

void TimingWheel::insert_timer(Timer* timer) {
    int64_t expire = timer->expire_ns;
    int64_t now = current_time_.load(std::memory_order_relaxed);

    expire = std::max(expire, now);

    // Calculate which level and bucket
    int64_t diff = expire - now;
    uint32_t level = 0;

    // Find the appropriate level for this timer
    // Level covers tick_ns * 256^(level+1) time range
    for (uint32_t l = 0; l < num_levels_; ++l) {
        int64_t level_range = tick_ns_;
        for (uint32_t k = 0; k <= l; ++k) {
            level_range *= 256;
        }
        if (diff < level_range) {
            level = l;
            break;
        }
    }

    // Calculate bucket index for this level
    int64_t level_offset = now / tick_ns_;
    for (uint32_t l = 0; l < level; ++l) {
        level_offset /= 256;
    }
    uint32_t bucket = static_cast<uint32_t>(level_offset) & levels_[level].mask;

    timer->id |= (static_cast<uint64_t>(level) << 48); // Store level in high
                                                       // bits
    levels_[level].buckets[bucket].push_back(timer);
}

TimingWheel::Timer* TimingWheel::remove_timer(uint64_t timer_id) {
    uint32_t level = static_cast<uint32_t>(timer_id >> 48);
    timer_id &= 0xFFFFFFFFFFFFULL; // Mask to get actual ID

    if (level >= num_levels_) {
        return nullptr;
    }

    // Search all buckets at this level for the timer
    // This is O(buckets) but timers at higher levels are fewer
    for (auto& bucket : levels_[level].buckets) {
        for (auto it = bucket.begin(); it != bucket.end(); ++it) {
            if ((*it)->id == timer_id) {
                Timer* timer = *it;
                bucket.erase(it);
                return timer;
            }
        }
    }
    return nullptr;
}

uint32_t TimingWheel::advance(int64_t now_ns) {
    int64_t old_time = current_time_.load(std::memory_order_relaxed);
    if (now_ns <= old_time) {
        return 0;
    }

    current_time_.store(now_ns, std::memory_order_relaxed);

    uint32_t fired = 0;

    // Process all levels
    for (uint32_t level = 0; level < num_levels_; ++level) {
        uint64_t level_offset = static_cast<uint64_t>(old_time / tick_ns_);
        for (uint32_t l = 0; l < level; ++l) {
            level_offset /= 256;
        }
        uint32_t start_bucket =
            static_cast<uint32_t>(level_offset) & levels_[level].mask;

        uint64_t end_offset = static_cast<uint64_t>(now_ns / tick_ns_);
        for (uint32_t l = 0; l < level; ++l) {
            end_offset /= 256;
        }
        uint32_t end_bucket =
            static_cast<uint32_t>(end_offset) & levels_[level].mask;

        // Process buckets from start to end (wrapping around)
        uint32_t num_buckets = levels_[level].num_buckets;
        uint32_t count =
            ((end_bucket - start_bucket + num_buckets) % num_buckets) + 1;

        for (uint32_t i = 0; i < count; ++i) {
            uint32_t bucket_idx = (start_bucket + i) % num_buckets;
            auto& bucket = levels_[level].buckets[bucket_idx];

            for (auto it = bucket.begin(); it != bucket.end();) {
                Timer* timer = *it;
                if (timer->cancelled) {
                    it = bucket.erase(it);
                    delete timer;
                    continue;
                }

                if (timer->expire_ns <= now_ns) {
                    // Timer expired, fire it
                    it = bucket.erase(it);
                    timer->callback();
                    delete timer;
                    ++fired;
                } else {
                    // Timer not yet expired, might need to cascade to lower
                    // level
                    if (level > 0) {
                        // Recalculate which bucket this timer should be in
                        // at this (lower) level
                        uint32_t lower_level = level - 1;
                        int64_t lower_offset = now_ns / tick_ns_;
                        for (uint32_t l = 0; l <= lower_level; ++l) {
                            lower_offset /= 256;
                        }
                        uint32_t lower_bucket =
                            static_cast<uint32_t>(lower_offset) &
                            levels_[lower_level].mask;

                        timer->id &= 0xFFFFFFFFFFFFULL;
                        timer->id |= (static_cast<uint64_t>(lower_level) << 48);
                        bucket.erase(it);
                        levels_[lower_level].buckets[lower_bucket].push_back(timer);
                        it = bucket.begin(); // Reset since we erased
                        continue;
                    }
                    ++it;
                }
            }
        }
    }

    return fired;
}

bool TimingWheel::empty() const {
    for (const auto& level : levels_) {
        for (const auto& bucket : level.buckets) {
            if (!bucket.empty()) {
                return false;
            }
        }
    }
    return true;
}

size_t TimingWheel::size() const {
    size_t count = 0;
    for (const auto& level : levels_) {
        for (const auto& bucket : level.buckets) {
            count += bucket.size();
        }
    }
    return count;
}

} // namespace hpactor::sched