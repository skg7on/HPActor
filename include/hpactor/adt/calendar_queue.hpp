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

#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace hpactor::adt {

struct CalendarQueueConfig {
    int64_t fine_bucket_ns = 1'000'000;  // 1ms
    uint32_t fine_buckets = 256;         // power of 2
    uint32_t coarse_buckets = 256;       // power of 2
    uint32_t remote_buckets = 256;       // power of 2
    uint32_t max_advance_buckets = 4096; // ~4s cap per advance()
};

class CalendarQueue {
  public:
    using TimerCallback = std::function<void()>;

    explicit CalendarQueue(const CalendarQueueConfig& cfg = {});
    ~CalendarQueue();

    CalendarQueue(const CalendarQueue&) = delete;
    CalendarQueue& operator=(const CalendarQueue&) = delete;
    CalendarQueue(CalendarQueue&&) = delete;
    CalendarQueue& operator=(CalendarQueue&&) = delete;

    [[nodiscard]] uint64_t schedule(int64_t delay_ns, TimerCallback cb);
    [[nodiscard]] uint64_t schedule_at(int64_t expire_ns, TimerCallback cb);
    bool cancel(uint64_t timer_id);
    uint32_t advance(int64_t now_ns);

    bool empty() const;
    size_t size() const {
        return timer_map_.size();
    }

  private:
    struct Timer {
        int64_t expire_ns;
        uint64_t id;
        TimerCallback callback;
        Timer* next = nullptr;
        Timer* prev = nullptr;
        uint32_t bucket_idx = 0;
        uint8_t wheel_level = 0; // 0=fine, 1=coarse, 2=remote
    };

    struct BucketList {
        Timer* head = nullptr;
        Timer* tail = nullptr;
        uint32_t count = 0;

        void push_back(Timer* t);
        void unlink(Timer* t);
    };

    void insert_timer(Timer* timer, int64_t now_ns);
    void cascade_coarse(int64_t now_ns);
    void cascade_remote(int64_t now_ns);

    std::vector<BucketList> fine_wheel_;
    std::vector<BucketList> coarse_wheel_;
    std::vector<BucketList> remote_wheel_;
    std::unordered_map<uint64_t, Timer*> timer_map_;

    int64_t fine_bucket_ns_;
    int64_t coarse_bucket_ns_;
    int64_t remote_bucket_ns_;
    uint32_t fine_mask_;
    uint32_t coarse_mask_;
    uint32_t remote_mask_;
    uint32_t max_advance_buckets_;

    uint32_t current_fine_ = 0;
    uint32_t current_coarse_ = 0;
    uint32_t current_remote_ = 0;
    int64_t last_advance_ns_ = 0;

    std::atomic<uint64_t> next_id_{1};
    mutable std::recursive_mutex mutex_;
};

} // namespace hpactor::adt
