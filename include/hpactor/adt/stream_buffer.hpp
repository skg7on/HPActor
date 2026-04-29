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

#include <cstddef>
#include <cstdint>
#include <vector>

namespace hpactor {
namespace adt {

// StreamBuffer — contiguous byte buffer with O(1) consume and arena allocation.
//
// Combines ring-buffer semantics (read_pos_ offset tracks consumed prefix,
// consume() only advances a pointer) with pre-allocated arena backing
// (geometric growth from an initial 64 KB capacity). Lazy compaction means
// memmove is amortized to near-zero: it only triggers when read_pos_ exceeds
// half the live data, or when capacity must grow.
//
// Move-only. data() always returns a contiguous pointer, compatible with
// std::span and iovec consumers.
class StreamBuffer {
public:
    static constexpr size_t kDefaultInitialCapacity = 65536;
    static constexpr size_t kCompactThreshold = 65536;

    explicit StreamBuffer(size_t initial_capacity = kDefaultInitialCapacity);
    ~StreamBuffer() = default;

    // Move-only
    StreamBuffer(StreamBuffer&&) noexcept = default;
    StreamBuffer& operator=(StreamBuffer&&) noexcept = default;
    StreamBuffer(const StreamBuffer&) = delete;
    StreamBuffer& operator=(const StreamBuffer&) = delete;

    // ---- Capacity ----

    size_t size() const noexcept { return buf_.size() - read_pos_; }
    bool empty() const noexcept { return size() == 0; }
    size_t capacity() const noexcept { return buf_.capacity(); }

    // ---- Reserve + Write (direct I/O) ----

    // Ensure at least n bytes of writable space beyond the logical end.
    // Returns a pointer to the writable region. Call commit_tail(n_actual)
    // after writing to advance the logical end.
    uint8_t* reserve_tail(size_t n);

    // Advance the logical end by n bytes. Must be <= the amount reserved by
    // the most recent reserve_tail() call.
    void commit_tail(size_t n);

    // ---- Append (copy-in) ----

    void append(const uint8_t* data, size_t len);

    // ---- Access ----

    // data() may trigger lazy compaction. Returns contiguous pointer to the
    // start of readable data.
    uint8_t* data() noexcept {
        maybe_compact();
        return buf_.data() + read_pos_;
    }
    const uint8_t* data() const noexcept { return buf_.data() + read_pos_; }

    uint8_t& operator[](size_t i) { return buf_[read_pos_ + i]; }
    const uint8_t& operator[](size_t i) const { return buf_[read_pos_ + i]; }

    // ---- Consume ----

    // Advance the read position by n bytes. O(1), no memmove.
    void consume(size_t n);

    // Discard all data and reset offsets.
    void clear() noexcept;

    // Explicitly compact: shift live data to front of the backing store.
    void compact();

private:
    void ensure_capacity(size_t total_needed);
    void maybe_compact();

    std::vector<uint8_t> buf_;
    size_t read_pos_ = 0;
};

} // namespace adt
} // namespace hpactor
