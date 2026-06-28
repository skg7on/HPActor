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

namespace hpactor {

class ActorDirectory;
class ActorSystem;

namespace mailbox {
class DeadLetterQueue;
}

namespace sched {

/// \brief Fixed concrete dependencies for scheduler execution.
///
/// Replaces stored \c ActorSystem& references on scheduler hot paths
/// with only the specific objects they use: actor directory, dead-letter
/// sink, and immutable coroutine mode.
struct ActorExecutionDependencies final {
    ActorDirectory& actors;                 ///< For actor/mailbox lookup.
    mailbox::DeadLetterQueue* dead_letters; ///< For expired message DLQ
                                            ///< (nullable).
    bool use_coroutines;                    ///< Immutable coroutine flag.

    /// \brief Build from a facade for source-compatible construction.
    static ActorExecutionDependencies from(ActorSystem& system) noexcept;
};

} // namespace sched
} // namespace hpactor
