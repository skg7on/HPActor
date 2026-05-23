// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace hpactor {

enum class LifecycleState : uint8_t {
    kStarting = 0,
    kActive = 1,
    kDraining = 2,
    kStopping = 3,
    kStopped = 4,
    kFailed = 5,
    kRecovering = 6,
    kQuarantined = 7, ///< Isolated — rejects user messages, accepts system
                      ///< messages.
};

struct StateDef {
    LifecycleState state;
    const char* name;
    bool accepts_user_msgs : 1;
    bool accepts_system_msgs : 1;
    uint8_t num_transitions : 3;
    LifecycleState transitions[7];
};

constexpr StateDef kStateMachine[] = {
    {LifecycleState::kStarting,
     "starting",
     false,
     true,
     1,
     {LifecycleState::kActive, LifecycleState::kFailed}},
    {LifecycleState::kActive,
     "active",
     true,
     true,
     4,
     {LifecycleState::kDraining, LifecycleState::kStopping,
      LifecycleState::kFailed, LifecycleState::kQuarantined}},
    {LifecycleState::kDraining,
     "draining",
     false,
     true,
     2,
     {LifecycleState::kStopping, LifecycleState::kFailed}},
    {LifecycleState::kStopping,
     "stopping",
     false,
     true,
     2,
     {LifecycleState::kStopped, LifecycleState::kFailed}},
    {LifecycleState::kStopped, "stopped", false, false, 1, {LifecycleState::kStarting}},
    {LifecycleState::kFailed,
     "failed",
     false,
     true,
     4,
     {LifecycleState::kStarting, LifecycleState::kStopped,
      LifecycleState::kRecovering, LifecycleState::kQuarantined}},
    {LifecycleState::kRecovering,
     "recovering",
     false,
     true,
     3,
     {LifecycleState::kActive, LifecycleState::kFailed, LifecycleState::kQuarantined}},
    {LifecycleState::kQuarantined, "quarantined", false, true, 1, {LifecycleState::kStopped}},
};

static_assert(sizeof(kStateMachine) / sizeof(StateDef) == 8, "kStateMachine "
                                                             "must have "
                                                             "exactly 8 "
                                                             "entries");

} // namespace hpactor
