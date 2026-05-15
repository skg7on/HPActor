// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>

namespace hpactor {

enum class DrainPolicy : uint8_t {
    Drain = 0,            // Process all mailbox messages before stopping
    DropUserMessages = 1, // Dead-letter user messages, keep system messages
    ImmediateStop = 2,    // Stop immediately, dead-letter everything
    SnapshotAndStop = 3,  // [DEFERRED] Durable actors persist state then stop
    TransferShard = 4,    // [DEFERRED] Sharded actors hand off ownership
};

} // namespace hpactor
