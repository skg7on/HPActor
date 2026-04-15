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

#include <hpactor/sched/edf_queue.hpp>

namespace hpactor::sched {

bool EDFQueue::later_deadline(const HeapItem& a, const HeapItem& b) {
    if (a.deadline != b.deadline) {
        return a.deadline > b.deadline;
    }
    // Same deadline: use FIFO sequence number
    // Smaller sequence = pushed earlier = should be returned first
    return a.sequence < b.sequence;
}

void EDFQueue::push(int64_t deadline_ns, WorkItem item) {
    HeapItem hi;
    hi.deadline = deadline_ns;
    hi.sequence = next_sequence_++;
    hi.item = std::move(item);

    items_.push_back(std::move(hi));
    sift_up(items_.size() - 1);
}

bool EDFQueue::pop(WorkItem& out) {
    if (items_.empty()) {
        return false;
    }

    out = std::move(items_[0].item);

    if (items_.size() == 1) {
        items_.pop_back();
        return true;
    }

    // Move last item to root and sift down
    items_[0] = std::move(items_.back());
    items_.pop_back();
    sift_down(0);

    return true;
}

bool EDFQueue::peek(int64_t& deadline_ns_out) const {
    if (items_.empty()) {
        return false;
    }
    deadline_ns_out = items_[0].deadline;
    return true;
}

void EDFQueue::sift_up(size_t i) {
    while (i > 0) {
        size_t p = parent(i);
        if (later_deadline(items_[p], items_[i])) {
            std::swap(items_[p], items_[i]);
            i = p;
        } else {
            break;
        }
    }
}

void EDFQueue::sift_down(size_t i) {
    size_t n = items_.size();

    while (true) {
        size_t smallest = i;
        size_t l = left(i);
        size_t r = right(i);

        if (l < n && later_deadline(items_[smallest], items_[l])) {
            smallest = l;
        }
        if (r < n && later_deadline(items_[smallest], items_[r])) {
            smallest = r;
        }

        if (smallest != i) {
            std::swap(items_[i], items_[smallest]);
            i = smallest;
        } else {
            break;
        }
    }
}

} // namespace hpactor::sched