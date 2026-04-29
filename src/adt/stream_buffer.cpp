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

#include <hpactor/adt/stream_buffer.hpp>

#include <algorithm>
#include <cstring>

namespace hpactor {
namespace adt {

StreamBuffer::StreamBuffer(size_t initial_capacity) {
    buf_.reserve(initial_capacity);
}

uint8_t* StreamBuffer::reserve_tail(size_t n) {
    ensure_capacity(n);
    return buf_.data() + buf_.size();
}

void StreamBuffer::commit_tail(size_t n) {
    buf_.resize(buf_.size() + n);
}

void StreamBuffer::append(const uint8_t* data, size_t len) {
    ensure_capacity(len);
    buf_.insert(buf_.end(), data, data + len);
}

void StreamBuffer::consume(size_t n) {
    read_pos_ += n;
}

void StreamBuffer::clear() noexcept {
    buf_.clear();
    read_pos_ = 0;
}

void StreamBuffer::compact() {
    if (read_pos_ == 0)
        return;
    size_t readable = buf_.size() - read_pos_;
    if (readable == 0) {
        buf_.clear();
        read_pos_ = 0;
        return;
    }
    std::memmove(buf_.data(), buf_.data() + read_pos_, readable);
    buf_.resize(readable);
    read_pos_ = 0;
}

void StreamBuffer::maybe_compact() {
    if (read_pos_ == 0)
        return;
    size_t readable = buf_.size() - read_pos_;
    if (read_pos_ > readable || read_pos_ > kCompactThreshold) {
        compact();
    }
}

void StreamBuffer::ensure_capacity(size_t additional_bytes) {
    // If consumed prefix occupies space at the front, compact to reclaim it
    if (read_pos_ > 0) {
        size_t usable = buf_.capacity() - buf_.size();
        if (usable < additional_bytes) {
            compact();
        }
    }
    // Grow geometrically if still insufficient
    size_t usable = buf_.capacity() - buf_.size();
    if (usable < additional_bytes) {
        size_t new_cap = std::max(buf_.capacity() * 2,
                                  buf_.size() + additional_bytes);
        new_cap = std::max(new_cap, kDefaultInitialCapacity);
        buf_.reserve(new_cap);
    }
}

} // namespace adt
} // namespace hpactor
