// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <hpactor/timer/timer_plane.hpp>

#include <algorithm>
#include <thread>

namespace hpactor::sched {

TimerPlane::TimerPlane(uint32_t num_shards, int64_t tick_ns)
    : num_shards_(num_shards > 0 ? num_shards : 1) {
    shards_.reserve(num_shards_);
    for (uint32_t i = 0; i < num_shards_; ++i) {
        shards_.emplace_back(std::make_unique<TimerPlaneShard>(i, tick_ns));
    }
}

uint32_t TimerPlane::select_shard() const {
    auto tid = std::hash<std::thread::id>{}(std::this_thread::get_id());
    return static_cast<uint32_t>(tid % num_shards_);
}

uint64_t TimerPlane::schedule(int64_t delay_ns, timer_callback cb) {
    uint32_t si = select_shard();
    auto& shard = *shards_[si];
    TimerHandle h = shard.schedule(delay_ns, std::move(cb));
    return h.valid() ? h.value() : 0;
}

bool TimerPlane::cancel(uint64_t timer_id) {
    TimerHandle h{timer_id};
    uint32_t si = TimerHandle::shard_index(h);
    if (si >= num_shards_) {
        return false;
    }
    return shards_[si]->cancel(h);
}

uint32_t TimerPlane::advance(int64_t now_ns) {
    uint32_t total = 0;
    for (auto& shard : shards_) {
        total += shard->advance(now_ns);
    }
    return total;
}

int64_t TimerPlane::next_deadline() const {
    int64_t earliest = INT64_MAX;
    for (const auto& shard : shards_) {
        int64_t dl = shard->min_deadline_ns();
        if (dl < earliest) {
            earliest = dl;
        }
    }
    return earliest;
}

bool TimerPlane::empty() const {
    for (const auto& shard : shards_) {
        if (shard->pending_count() > 0) {
            return false;
        }
    }
    return true;
}

size_t TimerPlane::size() const {
    size_t total = 0;
    for (const auto& shard : shards_) {
        total += shard->pending_count();
    }
    return total;
}

const TimerPlaneShard& TimerPlane::shard(uint32_t i) const {
    return *shards_[i];
}

TimerPlaneShard& TimerPlane::shard(uint32_t i) {
    return *shards_[i];
}

uint32_t TimerPlane::num_shards() const {
    return num_shards_;
}

} // namespace hpactor::sched
