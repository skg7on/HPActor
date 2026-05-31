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

namespace hpactor::mailbox::detail {

enum class ReservationResult : uint8_t {
    Reserved,
    CountCapacity,
    ByteCapacity,
};

template <typename T> class ReservationManager {
  public:
    ReservationManager() = default;

    // Two-phase reservation: count first, then bytes with rollback on failure.
    ReservationResult try_reserve(uint64_t bytes, uint32_t max_messages,
                                  uint64_t max_bytes) noexcept {
        // Phase 1: count reservation.
        if (max_messages > 0) {
            uint32_t cur = reserved_messages_.load(std::memory_order_acquire);
            do {
                if (cur >= max_messages)
                    return ReservationResult::CountCapacity;
            } while (!reserved_messages_.compare_exchange_weak(
                cur, cur + 1, std::memory_order_acq_rel, std::memory_order_acquire));
        }

        // Phase 2: byte budget reservation.
        if (max_bytes > 0) {
            uint64_t cur = queued_bytes_.load(std::memory_order_acquire);
            do {
                if (cur + bytes > max_bytes) {
                    if (max_messages > 0)
                        reserved_messages_.fetch_sub(1, std::memory_order_release);
                    return ReservationResult::ByteCapacity;
                }
            } while (!queued_bytes_.compare_exchange_weak(
                cur, cur + bytes, std::memory_order_acq_rel,
                std::memory_order_acquire));
            return ReservationResult::Reserved;
        }

        // Unlimited bytes: still track for observability.
        queued_bytes_.fetch_add(bytes, std::memory_order_release);
        return ReservationResult::Reserved;
    }

    void release(uint64_t bytes) noexcept {
        reserved_messages_.fetch_sub(1, std::memory_order_release);
        if (bytes > 0)
            queued_bytes_.fetch_sub(bytes, std::memory_order_release);
    }

    // Direct atomic access for inject_for_test bypass.
    void inject_count(uint64_t bytes) noexcept {
        reserved_messages_.fetch_add(1, std::memory_order_relaxed);
        queued_bytes_.fetch_add(bytes, std::memory_order_release);
    }

    uint32_t reserved_count() const noexcept {
        return reserved_messages_.load(std::memory_order_acquire);
    }
    uint64_t queued_bytes() const noexcept {
        return queued_bytes_.load(std::memory_order_acquire);
    }

  private:
    std::atomic<uint32_t> reserved_messages_{0};
    std::atomic<uint64_t> queued_bytes_{0};
};

} // namespace hpactor::mailbox::detail
