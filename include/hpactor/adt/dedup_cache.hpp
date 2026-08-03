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

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <memory>

namespace hpactor::adt {

/// \brief Bounded receiver-side deduplication cache for at-least-once
///        delivery.
///
/// Keyed on (source endpoint, source actor, message id). Entries expire
/// after a configurable TTL. When the cache is full, oldest entries are
/// evicted — this is safe because at-least-once semantics already allow
/// spurious duplicates.
///
/// \note Thread safety: all public methods are safe for concurrent
///       callers. Internal synchronization uses a mutex.
class DedupCache {
  public:
    struct Config {
        /// Maximum number of entries before eviction.
        size_t max_entries = 1024 * 64;
        /// Time-to-live for each entry in nanoseconds (default 5 minutes).
        uint64_t ttl_ns = 300'000'000'000ULL;
    };

    explicit DedupCache(Config cfg);
    ~DedupCache();

    DedupCache(const DedupCache&) = delete;
    DedupCache& operator=(const DedupCache&) = delete;
    DedupCache(DedupCache&&) noexcept;
    DedupCache& operator=(DedupCache&&) noexcept;

    /// \brief Check-and-set: returns true if the key was already seen
    ///        (duplicate), false if the key is new (and inserts it).
    ///
    /// \param[in] source_node The source node endpoint.
    /// \param[in] source_actor The source actor id.
    /// \param[in] message_id The message identifier.
    /// \return true if this (source_node, source_actor, message_id) tuple
    ///         was already seen within the TTL window.
    [[nodiscard]] bool is_duplicate(EndPoint source_node, ActorId source_actor,
                                    MessageId message_id) noexcept;

    /// \brief Remove expired entries.
    ///
    /// \param[in] now_ns Current monotonic timestamp in nanoseconds.
    void purge_expired(uint64_t now_ns) noexcept;

    /// \brief Approximate current entry count.
    [[nodiscard]] size_t size() const noexcept;

    /// \brief Number of duplicate hits since creation.
    [[nodiscard]] uint64_t duplicate_hits() const noexcept;

    /// \brief Number of insertions (non-duplicates) since creation.
    [[nodiscard]] uint64_t insertions() const noexcept;

    /// \brief Roll back a prior insertion.
    ///
    /// Removes the entry for (source_node, source_actor, message_id) if
    /// present. Used when a message was admitted by the dedup check but
    /// subsequently rejected (e.g., expired or mailbox full), so that
    /// a retry is not permanently suppressed.
    ///
    /// No-op if the key is not found.
    void remove(EndPoint source_node, ActorId source_actor,
                MessageId message_id) noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace hpactor::adt
