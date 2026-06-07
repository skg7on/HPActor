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

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/types/types.hpp>

#include <memory>

namespace hpactor {

class AbstractActor;
class ActorContext;

/// \brief Resolves target actors and enqueues messages into their mailboxes.
///
/// Decouples message routing (actor lookup) from the \c ActorSystem so
/// that delivery path behavior can be tested and evolved independently.
///
/// \note Thread safety: Not internally synchronized. Callers must ensure
///       that the referenced \c ActorDirectory outlives this engine and
///       that concurrent calls are externally synchronized or routed
///       through a single scheduler thread.
class LocalDeliveryEngine {
  public:
    /// \brief Construct with a reference to the actor directory.
    ///
    /// \param[in] directory The actor directory used for target resolution.
    /// \pre \p directory must outlive this engine.
    explicit LocalDeliveryEngine(ActorDirectory& directory);

    /// \brief Enqueue a message directly into the target actor's mailbox.
    ///
    /// Looks up the target mailbox in the directory. If the target is
    /// found, the message is placed into the mailbox and the result is
    /// \c Accepted. If the target is not found, the result is
    /// \c ActorNotFound.
    ///
    /// \param[in] target Actor ID to deliver to.
    /// \param[in] msg Message to deliver (ownership transferred).
    /// \return \c EnqueueResult with code and target ID.
    /// \retval Accepted Message was enqueued.
    /// \retval ActorNotFound No actor with id \p target is registered.
    mailbox::EnqueueResult
    try_deliver(ActorId target, std::unique_ptr<TypedMessage> msg);

  private:
    ActorDirectory& directory_;
};

} // namespace hpactor
