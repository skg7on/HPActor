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