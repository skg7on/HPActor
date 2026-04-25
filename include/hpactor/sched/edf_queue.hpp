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

#include <hpactor/sched/work_queue.hpp>

#include <algorithm>
#include <cstdint>
#include <vector>

namespace hpactor::sched {

// -----------------------------------------------------------------------------
// EDFQueue: Earliest Deadline First priority queue
// -----------------------------------------------------------------------------
// Orders work items by deadline_ns (earliest deadline first).
// Items with INT64_MAX deadline are treated as lowest priority (background).
//
// Uses a min-heap for O(log n) insert and extract.
// -----------------------------------------------------------------------------
class EDFQueue {
  public:
    EDFQueue() = default;

    EDFQueue(const EDFQueue&) = delete;
    EDFQueue& operator=(const EDFQueue&) = delete;
    EDFQueue(EDFQueue&&) = delete;
    EDFQueue& operator=(EDFQueue&&) = delete;

    // Insert an item with given deadline
    void push(int64_t deadline_ns, WorkItem item);

    // Extract the item with earliest deadline
    // Returns false if queue is empty
    bool pop(WorkItem& out);

    // Peek at earliest deadline without removing
    // Returns false if empty
    bool peek(int64_t& deadline_ns_out) const;

    // Check if queue is empty
    bool empty() const {
        return items_.empty();
    }

    // Number of items in queue
    size_t size() const {
        return items_.size();
    }

    // Clear the queue
    void clear() {
        items_.clear();
    }

  private:
    // Min-heap: parent has earlier deadline than children
    // Stored as vector of (deadline, sequence, WorkItem)
    struct HeapItem {
        int64_t deadline;
        uint64_t sequence; // FIFO tiebreaker for same deadline
        WorkItem item;
    };

    std::vector<HeapItem> items_;

    // Heap helper functions
    size_t parent(size_t i) const {
        return (i - 1) / 2;
    }
    size_t left(size_t i) const {
        return 2 * i + 1;
    }
    size_t right(size_t i) const {
        return 2 * i + 2;
    }

    void sift_up(size_t i);
    void sift_down(size_t i);

    // Sequence counter for FIFO ordering of equal deadlines
    uint64_t next_sequence_{0};

    // Comparator: returns true if a has later deadline than b
    static bool later_deadline(const HeapItem& a, const HeapItem& b);
};

} // namespace hpactor::sched