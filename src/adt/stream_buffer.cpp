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

// ---- Constructors ----

StreamBuffer::StreamBuffer(size_t count)
    : buf_(count, uint8_t{0}), read_pos_(0) {}

StreamBuffer::StreamBuffer(std::initializer_list<uint8_t> ilist)
    : buf_(ilist), read_pos_(0) {}

StreamBuffer::StreamBuffer(size_t count, uint8_t value)
    : buf_(count, value), read_pos_(0) {}

StreamBuffer StreamBuffer::with_capacity(size_t cap) {
    StreamBuffer sb;
    sb.buf_.reserve(cap);
    return sb;
}

// ---- Copy ----

StreamBuffer::StreamBuffer(const StreamBuffer& other)
    : buf_(other.buf_), read_pos_(other.read_pos_) {}

StreamBuffer& StreamBuffer::operator=(const StreamBuffer& other) {
    if (this != &other) {
        buf_ = other.buf_;
        read_pos_ = other.read_pos_;
    }
    return *this;
}

// ---- Capacity ----

void StreamBuffer::reserve(size_t n) {
    if (n > buf_.capacity()) {
        buf_.reserve(n);
    }
}

void StreamBuffer::resize(size_t n) {
    compact();
    buf_.resize(n);
}

void StreamBuffer::resize(size_t n, uint8_t value) {
    compact();
    buf_.resize(n, value);
}

// ---- Reserve + Write (direct I/O) ----

uint8_t* StreamBuffer::reserve_tail(size_t n) {
    ensure_capacity(n);
    return buf_.data() + buf_.size();
}

void StreamBuffer::commit_tail(size_t n) {
    buf_.resize(buf_.size() + n);
}

// ---- Append (copy-in) ----

void StreamBuffer::append(const uint8_t* data, size_t len) {
    ensure_capacity(len);
    buf_.insert(buf_.end(), data, data + len);
}

// ---- Modifiers ----

void StreamBuffer::push_back(uint8_t value) {
    ensure_capacity(1);
    buf_.push_back(value);
}

void StreamBuffer::assign(size_t count, uint8_t value) {
    clear();
    buf_.assign(count, value);
}

void StreamBuffer::assign(std::initializer_list<uint8_t> ilist) {
    clear();
    buf_.assign(ilist);
}

StreamBuffer::iterator StreamBuffer::insert(const_iterator pos,
                                            std::initializer_list<uint8_t> ilist) {
    size_t offset = static_cast<size_t>(pos - (buf_.data() + read_pos_));
    compact();
    auto it = buf_.insert(buf_.begin() + static_cast<std::ptrdiff_t>(offset), ilist);
    return buf_.data() + static_cast<size_t>(it - buf_.begin());
}

StreamBuffer::iterator StreamBuffer::erase(const_iterator first, const_iterator last) {
    if (first >= last) return begin();

    size_t old_read_pos = read_pos_;
    const uint8_t* logical_begin = buf_.data() + old_read_pos;
    size_t offset_first = static_cast<size_t>(first - logical_begin);
    size_t offset_last = static_cast<size_t>(last - logical_begin);

    if (offset_first == 0) {
        consume(offset_last - offset_first);
        return begin();
    }

    compact();
    auto it = buf_.erase(buf_.begin() + static_cast<std::ptrdiff_t>(offset_first),
                         buf_.begin() + static_cast<std::ptrdiff_t>(offset_last));
    return buf_.data() + static_cast<size_t>(it - buf_.begin());
}

void StreamBuffer::clear() noexcept {
    buf_.clear();
    read_pos_ = 0;
}

// ---- Consume ----

void StreamBuffer::consume(size_t n) {
    read_pos_ += n;
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

// ---- Comparison ----

bool StreamBuffer::operator==(const StreamBuffer& other) const {
    if (size() != other.size()) return false;
    if (size() == 0) return true;
    return std::memcmp(data(), other.data(), size()) == 0;
}

// ---- Private ----

void StreamBuffer::maybe_compact() {
    if (read_pos_ == 0)
        return;
    size_t readable = buf_.size() - read_pos_;
    if (read_pos_ > readable || read_pos_ > kCompactThreshold) {
        compact();
    }
}

void StreamBuffer::ensure_capacity(size_t additional_bytes) {
    if (read_pos_ > 0) {
        size_t usable = buf_.capacity() - buf_.size();
        if (usable < additional_bytes) {
            compact();
        }
    }
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
