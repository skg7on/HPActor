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

// Actor reference implementation - see actor_ref.hpp

#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/delivery_result.hpp>
#include <hpactor/ref/actor_ref.hpp>

namespace hpactor {

void ActorRef::send(const ActorAddress& target, TypedMessage msg) {
    (void)try_send(target, std::move(msg));
}

mailbox::DeliveryResult
ActorRef::try_send(const ActorAddress& target, TypedMessage msg,
                   mailbox::DeliveryOptions options) {
    if (is_local()) {
        Actor* actor = get_actor();
        if (actor != nullptr) {
            auto er = actor->get()->system().try_deliver_local(
                target.id, std::move(msg), /*priority=*/0,
                /*deadline_ns=*/INT64_MAX, options);
            return mailbox::DeliveryResult::from_enqueue(
                er, target, MessageId{options.message_id});
        }
        return {mailbox::DeliveryStatus::NoRoute, target,
                MessageId{options.message_id}, 0};
    } else {
        ActorProxy* proxy = get_proxy();
        if (proxy != nullptr) {
            return proxy->try_send(target, std::move(msg), options);
        }
        return {mailbox::DeliveryStatus::NoRoute, target,
                MessageId{options.message_id}, 0};
    }
}

} // namespace hpactor
