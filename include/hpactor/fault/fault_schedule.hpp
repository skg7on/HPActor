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

#include <hpactor/fault/fault_types.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <random>
#include <string>
#include <variant>
#include <vector>

namespace hpactor::fault {

/// \brief Payload for \c FaultAction::kDelay — stalls the domain tick counter.
struct DelayPayload {
    uint64_t ticks;
}; ///< Number of ticks to stall.

/// \brief Payload for \c FaultAction::kCorrupt — bit-level data corruption.
struct CorruptPayload {
    uint64_t byte_offset; ///< Byte offset into the target buffer or state.
    uint8_t byte_mask;    ///< XOR mask applied at the offset.
};

/// \brief Payload for \c FaultAction::kFail — synthetic error return.
struct FailPayload {
    int32_t error_code;
}; ///< Error code returned to the caller.

/// \brief Variant payload carried by a schedule entry.
///
/// The active alternative depends on the entry's \c FaultAction:
/// - \c kFail → \c FailPayload
/// - \c kDelay → \c DelayPayload
/// - \c kCorrupt → \c CorruptPayload
/// - \c kDrop / \c kPanic → \c std::monostate
using FaultPayload =
    std::variant<std::monostate, FailPayload, DelayPayload, CorruptPayload>;

/// \brief A single entry in a deterministic fault-injection schedule.
///
/// Each entry specifies exactly when (domain + tick), where (path), what
/// (action + payload), and optionally which actor (\p target) should be
/// affected.
struct FaultScheduleEntry {
    FaultDomain domain; ///< Subsystem domain whose tick counter is matched.
    uint64_t at_tick;   ///< Tick value the domain counter must reach.
    std::string path;   ///< Exact dot-separated injection site path.
    FaultAction action; ///< Action to take when the entry fires.
    std::optional<ActorId> target; ///< If set, only fire for this specific
                                   ///< actor.
    FaultPayload payload;          ///< Action-specific payload.
};

/// \brief A pre-computed, sorted schedule of fault injection events.
///
/// Schedules are built either programmatically via \c Builder / \c add_entry()
/// or expanded stochastically via \c expand_random(). The schedule is sorted
/// by (domain, tick) and consumed sequentially by \c FaultController::check().
///
/// \note Schedules are shared via \c std::shared_ptr so that
///       \c FaultController::check() holds a stable snapshot even when
///       \c load() swaps in a new schedule concurrently.
class FaultSchedule {
  public:
    /// \brief Fluent builder for adding entries to a schedule.
    class Builder;

    /// \brief Construct an empty schedule.
    FaultSchedule() = default;

    /// \brief Append a single entry to the schedule.
    ///
    /// \param[in] entry The entry to append.
    void add_entry(FaultScheduleEntry entry);

    /// \brief Remove all entries from the schedule.
    void clear();

    /// \brief Sort entries by ascending (domain, at_tick).
    ///
    /// Must be called before the schedule is loaded into a \c FaultController.
    void sort();

    /// \brief Expand the schedule with randomly placed entries.
    ///
    /// For each tick from 0 to \p max_ticks - 1, inserts an entry with
    /// probability \p probability. The RNG determines reproducibility;
    /// use a seeded engine for deterministic schedules.
    ///
    /// \tparam RNG A standard random number engine (e.g. \c std::mt19937).
    /// \param[in] domain Subsystem domain for all generated entries.
    /// \param[in] path Injection site path.
    /// \param[in] action Fault action for all generated entries.
    /// \param[in] probability Per-tick insertion probability in [0, 1].
    /// \param[in] max_ticks Upper bound (exclusive) on generated tick values.
    /// \param[in] rng Random number engine reference.
    /// \param[in] payload Action-specific payload (default: empty).
    /// \param[in] target Optional actor filter (default: all actors).
    /// \return Reference to \c *this for chaining.
    template <typename RNG>
    FaultSchedule&
    expand_random(FaultDomain domain, std::string_view path, FaultAction action,
                  double probability, uint64_t max_ticks, RNG& rng,
                  FaultPayload payload = {},
                  std::optional<ActorId> target = std::nullopt);

    /// \brief Return the sorted entry vector.
    const std::vector<FaultScheduleEntry>& entries() const noexcept {
        return entries_;
    }
    /// \brief Return \c true if the schedule has no entries.
    bool empty() const noexcept {
        return entries_.empty();
    }
    /// \brief Return the number of entries in the schedule.
    size_t size() const noexcept {
        return entries_.size();
    }

  private:
    std::vector<FaultScheduleEntry> entries_;
};

/// \brief Fluent builder for adding fault entries at a specific (domain, tick).
///
/// Each method appends one entry to the owning \c FaultSchedule and returns
/// \c *this for chaining. Use \c add_entry_to() to create a builder.
class FaultSchedule::Builder {
  public:
    /// \brief Construct a builder bound to a schedule, domain, and tick.
    ///
    /// \param[in] schedule The schedule entries will be appended to.
    /// \param[in] domain Subsystem domain for all entries.
    /// \param[in] at_tick Tick value for all entries.
    explicit Builder(FaultSchedule& schedule, FaultDomain domain, uint64_t at_tick)
        : schedule_(&schedule), domain_(domain), at_tick_(at_tick) {}

    /// \brief Append a \c kFail entry with the given error code.
    Builder& fail(std::string_view path, int32_t error_code);
    /// \brief Append a \c kDrop entry.
    Builder& drop(std::string_view path);
    /// \brief Append a \c kDelay entry with the given tick delay.
    Builder& delay(std::string_view path, uint64_t ticks);
    /// \brief Append a \c kCorrupt entry with byte offset and XOR mask.
    Builder&
    corrupt(std::string_view path, uint64_t byte_offset, uint8_t byte_mask);
    /// \brief Append a \c kPanic entry (calls \c std::abort on fire).
    Builder& panic(std::string_view path);
    /// \brief Set an optional actor filter for the next entry.
    ///
    /// \param[in] actor If set, the entry only fires for this specific actor.
    Builder& target(ActorId actor);

  private:
    FaultSchedule* schedule_;
    FaultDomain domain_;
    uint64_t at_tick_;
    std::optional<ActorId> target_;
};

/// \brief Create a \c FaultSchedule::Builder bound to \p schedule.
///
/// Convenience entry point for the fluent builder API.
///
/// \param[in] schedule The schedule to append entries to.
/// \param[in] domain Subsystem domain for all entries.
/// \param[in] at_tick Tick value for all entries.
/// \return A \c Builder ready for chaining.
inline FaultSchedule::Builder
add_entry_to(FaultSchedule& schedule, FaultDomain domain, uint64_t at_tick) {
    return FaultSchedule::Builder(schedule, domain, at_tick);
}

} // namespace hpactor::fault
