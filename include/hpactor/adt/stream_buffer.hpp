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
#include <initializer_list>
#include <type_traits>
#include <vector>

namespace hpactor {
namespace adt {

// StreamBuffer — contiguous byte buffer with O(1) consume and arena allocation.
//
// Combines ring-buffer semantics (read_pos_ offset tracks consumed prefix,
// consume() only advances a pointer) with pre-allocated arena backing
// (geometric growth from an initial capacity). Lazy compaction means
// memmove is amortized to near-zero: it only triggers when read_pos_ exceeds
// half the live data, or when capacity must grow.
//
// Provides a std::vector<uint8_t>-compatible API so it can serve as the
// concrete type behind the `bytes` typedef.
class StreamBuffer {
  public:
    using value_type = uint8_t;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = uint8_t&;
    using const_reference = const uint8_t&;
    using pointer = uint8_t*;
    using const_pointer = const uint8_t*;
    using iterator = pointer;
    using const_iterator = const_pointer;

    static constexpr size_t kDefaultInitialCapacity = 65536;
    static constexpr size_t kCompactThreshold = 65536;

    // ---- Constructors ----

    StreamBuffer() = default;

    // Create a buffer with count zero-initialized bytes (matches std::vector).
    explicit StreamBuffer(size_t count);

    StreamBuffer(std::initializer_list<uint8_t> ilist);
    StreamBuffer(size_t count, uint8_t value);

    template <typename InputIt, typename = std::enable_if_t<!std::is_integral_v<InputIt>>>
    StreamBuffer(InputIt first, InputIt last);

    ~StreamBuffer() = default;

    // Create a buffer with pre-allocated capacity but size() == 0.
    static StreamBuffer with_capacity(size_t cap);

    /// \brief Create a buffer from raw data with exact-fit capacity.
    ///
    /// Unlike the iterator-pair constructor, this factory creates a buffer
    /// whose initial capacity matches the data size, avoiding the 64 KB
    /// default minimum. The data is copied into the buffer.
    ///
    /// \param[in] data Pointer to the source bytes. May be \c nullptr when
    ///                 \p len is 0.
    /// \param[in] len  Number of bytes to copy.
    /// \return A \c StreamBuffer containing a copy of \p data with capacity
    ///         sized to \p len.
    static StreamBuffer from_data(const uint8_t* data, size_t len);

    // ---- Copy ----

    StreamBuffer(const StreamBuffer& other);
    StreamBuffer& operator=(const StreamBuffer& other);

    // ---- Move ----

    StreamBuffer(StreamBuffer&&) noexcept = default;
    StreamBuffer& operator=(StreamBuffer&&) noexcept = default;

    // ---- Capacity ----

    size_t size() const noexcept {
        return buf_.size() - read_pos_;
    }
    bool empty() const noexcept {
        return size() == 0;
    }
    size_t capacity() const noexcept {
        return buf_.capacity();
    }
    void reserve(size_t n);
    void resize(size_t n);
    void resize(size_t n, uint8_t value);

    // ---- Iterators ----

    iterator begin() noexcept {
        maybe_compact();
        return buf_.data() + read_pos_;
    }
    const_iterator begin() const noexcept {
        return buf_.data() + read_pos_;
    }
    const_iterator cbegin() const noexcept {
        return begin();
    }
    iterator end() noexcept {
        return begin() + size();
    }
    const_iterator end() const noexcept {
        return begin() + size();
    }
    const_iterator cend() const noexcept {
        return end();
    }

    // ---- Element access ----

    uint8_t& front() {
        return operator[](0);
    }
    const uint8_t& front() const {
        return operator[](0);
    }
    uint8_t& back() {
        return operator[](size() - 1);
    }
    const uint8_t& back() const {
        return operator[](size() - 1);
    }

    uint8_t& operator[](size_t i) {
        return buf_[read_pos_ + i];
    }
    const uint8_t& operator[](size_t i) const {
        return buf_[read_pos_ + i];
    }

    uint8_t* data() noexcept {
        maybe_compact();
        return buf_.data() + read_pos_;
    }
    const uint8_t* data() const noexcept {
        return buf_.data() + read_pos_;
    }

    // ---- Reserve + Write (direct I/O) ----

    uint8_t* reserve_tail(size_t n);
    void commit_tail(size_t n);

    // ---- Append (copy-in) ----

    void append(const uint8_t* data, size_t len);

    // ---- Modifiers ----

    void push_back(uint8_t value);

    void assign(size_t count, uint8_t value);
    template <typename InputIt> void assign(InputIt first, InputIt last);
    void assign(std::initializer_list<uint8_t> ilist);

    template <typename InputIt>
    iterator insert(const_iterator pos, InputIt first, InputIt last);

    iterator insert(const_iterator pos, std::initializer_list<uint8_t> ilist);

    iterator erase(const_iterator first, const_iterator last);

    void clear() noexcept;

    // ---- Consume (StreamBuffer-specific, not in std::vector) ----

    void consume(size_t n);
    void compact();

    // ---- Comparison ----

    bool operator==(const StreamBuffer& other) const;
    bool operator!=(const StreamBuffer& other) const {
        return !(*this == other);
    }

  private:
    void ensure_capacity(size_t additional_bytes);
    void maybe_compact();

    std::vector<uint8_t> buf_;
    size_t read_pos_ = 0;
    size_t reserve_tail_amount_ = 0;
};

// ---- Template implementations ----

template <typename InputIt, typename>
StreamBuffer::StreamBuffer(InputIt first, InputIt last) {
    buf_.insert(buf_.end(), first, last);
}

template <typename InputIt>
void StreamBuffer::assign(InputIt first, InputIt last) {
    clear();
    buf_.assign(first, last);
}

template <typename InputIt>
auto StreamBuffer::insert(const_iterator pos, InputIt first, InputIt last)
    -> iterator {
    size_t offset = static_cast<size_t>(pos - (buf_.data() + read_pos_));
    compact();
    auto it = buf_.insert(buf_.begin() + static_cast<std::ptrdiff_t>(offset),
                          first, last);
    return buf_.data() + static_cast<size_t>(it - buf_.begin());
}

} // namespace adt
} // namespace hpactor
