// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <cstdint>
#include <hpactor/sched/scheduler_interfaces.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::sched {

/// Intrusive timer node stored in per-shard wheel buckets.
/// Doubly-linked for O(1) unlink on cancel.
struct TimerNode {
    TimerNode* next{nullptr};
    TimerNode* prev{nullptr};
    int64_t expire_ns{0};
    uint32_t slot_index{0};
    uint8_t generation{0};
    uint64_t group_handle{0}; ///< Owning TimerGroup handle, 0 = none
    uint8_t priority{0};
    uint8_t wheel_level{0}; ///< Current wheel level (0 = finest)
    uint32_t bucket_idx{0}; ///< Bucket index within that level
    TraceContext trace;
    timer_callback callback; ///< std::function<void()> callback on fire
};

/// Command sent across the MPSC command queue for cross-thread timer ops.
struct TimerCommand {
    enum class Type : uint8_t {
        Schedule = 0,
        Cancel = 1,
        DrainGroup = 2,
    };

    Type type{Type::Schedule};

    union {
        struct {
            int64_t expire_ns;
            TimerNode* node; ///< Pre-allocated node (caller owns until pushed)
        } schedule;

        struct {
            uint64_t handle; ///< Encoded TimerHandle value
        } cancel;

        struct {
            uint64_t group_handle;
        } drain_group;
    };

    static TimerCommand make_schedule(int64_t exp, TimerNode* n) {
        TimerCommand c;
        c.type = Type::Schedule;
        c.schedule.expire_ns = exp;
        c.schedule.node = n;
        return c;
    }

    static TimerCommand make_cancel(uint64_t h) {
        TimerCommand c;
        c.type = Type::Cancel;
        c.cancel.handle = h;
        return c;
    }

    static TimerCommand make_drain_group(uint64_t gh) {
        TimerCommand c;
        c.type = Type::DrainGroup;
        c.drain_group.group_handle = gh;
        return c;
    }
};

} // namespace hpactor::sched
