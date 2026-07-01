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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/mailbox/fixed_mailbox_interface.hpp>
#include <hpactor/mailbox/local_delivery_engine.hpp>
#include <hpactor/mailbox/mailbox_kind.hpp>
#include <hpactor/mailbox/mpsc_actor_mailbox.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor {

LocalDeliveryEngine::LocalDeliveryEngine(ActorDirectory& directory)
    : directory_(directory) {}

mailbox::EnqueueResult
LocalDeliveryEngine::try_deliver(ActorId target, TypedMessage msg) {
    auto entry = directory_.find(target);
    if (!entry.has_value()) {
        return mailbox::EnqueueResult{
            .code = mailbox::EnqueueResultCode::ActorNotFound, .target = target};
    }

    // Fixed-mailbox actors: system messages through control port,
    // user messages rejected with UnsupportedMessageType.
    if (entry->mailbox_kind == mailbox::MailboxKind::FixedDisruptor) {
        if (static_cast<uint32_t>(msg.type_id()) >=
            static_cast<uint32_t>(TypeTag::User)) {
            mailbox::EnqueueResult result;
            result.code = mailbox::EnqueueResultCode::Rejected;
            result.target = target;
            return result;
        }
        if (entry->fixed_mailbox.control.try_push) {
            return entry->fixed_mailbox.control.try_push(
                entry->fixed_mailbox.control.context, std::move(msg));
        }
        return mailbox::EnqueueResult{
            .code = mailbox::EnqueueResultCode::Rejected, .target = target};
    }

    // Default path: variable MPSC mailbox.
    auto mailbox = directory_.find_mailbox(target);
    if (!mailbox) {
        return mailbox::EnqueueResult{
            .code = mailbox::EnqueueResultCode::ActorNotFound, .target = target};
    }
    mailbox::MailboxEnvelopeMeta meta;
    meta.sender = msg.sender_address();
    meta.type_tag = msg.type_id();
    meta.priority = 0;
    meta.deadline_ns = INT64_MAX;
    auto* mbox =
        static_cast<mailbox::MPSCActorMailbox<TypedMessage>*>(mailbox.get());
    auto result = mbox->try_push(std::move(msg), meta);
    result.target = target;
    return result;
}

} // namespace hpactor
