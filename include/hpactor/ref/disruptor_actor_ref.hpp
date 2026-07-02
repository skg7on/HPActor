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

#include <hpactor/mailbox/disruptor_actor_mailbox.hpp>
#include <hpactor/mailbox/disruptor_message_envelope.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <cstdint>
#include <memory>

namespace hpactor {

class ActorContext;

/// \brief Typed local reference to a fixed-mailbox actor.
///
/// Carries the actor's address and a shared pointer to the stable
/// \c DisruptorActorMailboxCore.  The core outlives the actor, so a
/// surviving reference observes a clean rejection rather than a
/// dangling pointer.
///
/// \tparam Capacity Power-of-two ring capacity.
/// \tparam Messages The closed set of fixed user-message types.
template <size_t Capacity, mailbox::DisruptorMessage... Messages>
class DisruptorActorRef final {
  public:
    using core_type = mailbox::DisruptorActorMailboxCore<Capacity, Messages...>;

    DisruptorActorRef() noexcept = default;

    /// \brief Construct from an address and shared mailbox core.
    DisruptorActorRef(ActorAddress address, std::shared_ptr<core_type> core) noexcept
        : address_(address), core_(std::move(core)) {}

    /// \brief True when the reference has a valid target.
    [[nodiscard]] bool valid() const noexcept {
        return core_ != nullptr;
    }
    [[nodiscard]] explicit operator bool() const noexcept {
        return valid();
    }

    /// \brief The actor's address.
    [[nodiscard]] const ActorAddress& address() const noexcept {
        return address_;
    }

    /// \brief Try to send a declared user message.
    ///
    /// Only types in the closed set participate in overload resolution.
    /// Undeclared types produce a compile-time error.
    ///
    /// \return \c Accepted or a typed rejection.
    template <typename Message>
        requires mailbox::detail::one_of_v<std::remove_cvref_t<Message>, Messages...>
    [[nodiscard]] mailbox::EnqueueResult
    try_send(Message&& message,
             mailbox::DisruptorSendOptions options = {}) const noexcept {
        if (!core_) {
            mailbox::EnqueueResult result;
            result.code = mailbox::EnqueueResultCode::Rejected;
            result.target = address_.id;
            return result;
        }
        mailbox::DisruptorEnvelopeMeta meta;
        meta.deadline_ns = options.deadline_ns;
        meta.message_id = options.message_id;
        meta.flags = options.flags;
        return core_->try_push_user(std::forward<Message>(message), meta);
    }

  private:
    friend class ActorContext;

    /// \brief Send with pre-populated metadata (called by ActorContext).
    template <typename Message>
        requires mailbox::detail::one_of_v<std::remove_cvref_t<Message>, Messages...>
    [[nodiscard]] mailbox::EnqueueResult
    try_send_with_meta(Message&& message,
                       mailbox::DisruptorEnvelopeMeta meta) const noexcept {
        if (!core_) {
            mailbox::EnqueueResult result;
            result.code = mailbox::EnqueueResultCode::Rejected;
            result.target = address_.id;
            return result;
        }
        return core_->try_push_user(std::forward<Message>(message), meta);
    }

    ActorAddress address_;
    std::shared_ptr<core_type> core_;
};

} // namespace hpactor
