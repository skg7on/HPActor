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

#include <hpactor/adt/calendar_queue.hpp>

#include <algorithm>
#include <cstdlib>
#include <new>

namespace hpactor::adt {

// ---------------------------------------------------------------------------
// Timer lifecycle helpers
// ---------------------------------------------------------------------------

CalendarQueue::Timer*
CalendarQueue::make_timer(TimerCallback cb, int64_t expire_ns) {
    void* mem = make_storage_(sizeof(Timer));
    auto* timer = ::new (mem) Timer;
    timer->expire_ns = expire_ns;
    timer->id = next_id_.fetch_add(1, std::memory_order_relaxed);
    timer->callback = std::move(cb);
    return timer;
}

void CalendarQueue::destroy_timer(Timer* timer) {
    timer->~Timer();
    destroy_storage_(static_cast<void*>(timer), sizeof(Timer));
}

// ---------------------------------------------------------------------------
// BucketList
// ---------------------------------------------------------------------------

void CalendarQueue::BucketList::push_back(Timer* t) {
    t->next = nullptr;
    t->prev = tail;
    if (tail) {
        tail->next = t;
    } else {
        head = t;
    }
    tail = t;
    count++;
}

void CalendarQueue::BucketList::unlink(Timer* t) {
    if (t->prev) {
        t->prev->next = t->next;
    } else {
        head = t->next;
    }
    if (t->next) {
        t->next->prev = t->prev;
    } else {
        tail = t->prev;
    }
    t->next = nullptr;
    t->prev = nullptr;
    count--;
}

// ---------------------------------------------------------------------------
// CalendarQueue
// ---------------------------------------------------------------------------

CalendarQueue::CalendarQueue(const CalendarQueueConfig& cfg,
                             TimerStorageFactory make_storage,
                             TimerStorageDeleter destroy_storage)
    : fine_bucket_ns_(cfg.fine_bucket_ns),
      coarse_bucket_ns_(cfg.fine_bucket_ns * cfg.fine_buckets),
      remote_bucket_ns_(coarse_bucket_ns_ * cfg.coarse_buckets),
      max_advance_buckets_(cfg.max_advance_buckets),
      make_storage_(std::move(make_storage)),
      destroy_storage_(std::move(destroy_storage)) {
    if ((cfg.fine_buckets & (cfg.fine_buckets - 1)) != 0 ||
        (cfg.coarse_buckets & (cfg.coarse_buckets - 1)) != 0 ||
        (cfg.remote_buckets & (cfg.remote_buckets - 1)) != 0) {
        std::abort();
    }
    fine_mask_ = cfg.fine_buckets - 1;
    coarse_mask_ = cfg.coarse_buckets - 1;
    remote_mask_ = cfg.remote_buckets - 1;
    fine_wheel_.resize(cfg.fine_buckets);
    coarse_wheel_.resize(cfg.coarse_buckets);
    remote_wheel_.resize(cfg.remote_buckets);
}

CalendarQueue::~CalendarQueue() {
    for (auto& [id, timer] : timer_map_) {
        destroy_timer(timer);
    }
}

uint64_t CalendarQueue::schedule(int64_t delay_ns, TimerCallback cb) {
    int64_t now = last_advance_ns_;
    int64_t expire_ns = now + delay_ns;
    if (delay_ns <= 0) {
        expire_ns = now + fine_bucket_ns_;
    }
    return schedule_at(expire_ns, std::move(cb));
}

uint64_t CalendarQueue::schedule_at(int64_t expire_ns, TimerCallback cb) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto* timer = make_timer(std::move(cb), expire_ns);
    timer_map_[timer->id] = timer;
    int64_t now = last_advance_ns_;
    insert_timer(timer, now);
    return timer->id;
}

void CalendarQueue::insert_timer(Timer* timer, int64_t now_ns) {
    int64_t expire = std::max(timer->expire_ns, now_ns + fine_bucket_ns_);

    if (expire <
        now_ns + fine_bucket_ns_ * static_cast<int64_t>(fine_wheel_.size())) {
        uint32_t b = static_cast<uint32_t>(expire / fine_bucket_ns_) & fine_mask_;
        timer->bucket_idx = b;
        timer->wheel_level = 0;
        fine_wheel_[b].push_back(timer);
    } else if (expire < now_ns + coarse_bucket_ns_ *
                                     static_cast<int64_t>(coarse_wheel_.size())) {
        uint32_t b =
            static_cast<uint32_t>(expire / coarse_bucket_ns_) & coarse_mask_;
        timer->bucket_idx = b;
        timer->wheel_level = 1;
        coarse_wheel_[b].push_back(timer);
    } else {
        uint32_t b =
            static_cast<uint32_t>(expire / remote_bucket_ns_) & remote_mask_;
        timer->bucket_idx = b;
        timer->wheel_level = 2;
        remote_wheel_[b].push_back(timer);
    }
}

bool CalendarQueue::cancel(uint64_t timer_id) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    auto it = timer_map_.find(timer_id);
    if (it == timer_map_.end())
        return false;
    Timer* timer = it->second;
    timer_map_.erase(it);
    switch (timer->wheel_level) {
        case 0:
            fine_wheel_[timer->bucket_idx].unlink(timer);
            break;
        case 1:
            coarse_wheel_[timer->bucket_idx].unlink(timer);
            break;
        case 2:
            remote_wheel_[timer->bucket_idx].unlink(timer);
            break;
        default:
            break;
    }
    destroy_timer(timer);
    return true;
}

void CalendarQueue::cascade_coarse(int64_t now_ns) {
    auto& bucket = coarse_wheel_[current_coarse_];
    Timer* t = bucket.head;
    while (t) {
        Timer* next = t->next;
        bucket.unlink(t);
        insert_timer(t, now_ns);
        t = next;
    }
}

void CalendarQueue::cascade_remote(int64_t now_ns) {
    auto& bucket = remote_wheel_[current_remote_];
    Timer* t = bucket.head;
    while (t) {
        Timer* next = t->next;
        bucket.unlink(t);
        insert_timer(t, now_ns);
        t = next;
    }
}

uint32_t CalendarQueue::advance(int64_t now_ns) {
    // Collect expired timer callbacks under the lock, then fire them
    // outside the lock (same pattern as TimingWheel::advance).
    std::vector<TimerCallback> pending;

    {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (now_ns <= last_advance_ns_)
            return 0;
        if (last_advance_ns_ == 0) {
            last_advance_ns_ = now_ns;
            return 0;
        }

        // Cap the advance step (same rationale as TimingWheel::advance).
        static constexpr int64_t kMaxAdvanceNs = 100'000'000; // 100 ms
        if (now_ns - last_advance_ns_ > kMaxAdvanceNs) {
            now_ns = last_advance_ns_ + kMaxAdvanceNs;
        }

        uint32_t buckets_processed = 0;

        while (last_advance_ns_ + fine_bucket_ns_ <= now_ns &&
               buckets_processed < max_advance_buckets_) {
            auto& bucket = fine_wheel_[current_fine_];
            Timer* t = bucket.head;
            while (t) {
                Timer* next = t->next;
                bucket.unlink(t);
                timer_map_.erase(t->id);
                if (t->expire_ns <= now_ns) {
                    pending.push_back(std::move(t->callback));
                }
                destroy_timer(t);
                t = next;
            }

            last_advance_ns_ += fine_bucket_ns_;
            current_fine_ = (current_fine_ + 1) & fine_mask_;
            buckets_processed++;

            if (current_fine_ == 0) {
                cascade_coarse(now_ns);
                current_coarse_ = (current_coarse_ + 1) & coarse_mask_;
                if (current_coarse_ == 0) {
                    cascade_remote(now_ns);
                    current_remote_ = (current_remote_ + 1) & remote_mask_;
                }
            }
        }
    }
    // Lock released — fire callbacks safely outside the critical section.

    for (auto& cb : pending) {
        cb();
    }

    return static_cast<uint32_t>(pending.size());
}

bool CalendarQueue::empty() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);
    return timer_map_.empty();
}

} // namespace hpactor::adt
