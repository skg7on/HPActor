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
#include <hpactor/actor/local_delivery_engine.hpp>
#include <hpactor/mailbox/multi_lane_queue.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor {

LocalDeliveryEngine::LocalDeliveryEngine(ActorDirectory& directory)
    : directory_(directory) {}

mailbox::EnqueueResult
LocalDeliveryEngine::try_deliver(ActorId target, std::unique_ptr<TypedMessage> msg) {
    auto mailbox = directory_.find_mailbox(target);
    if (!mailbox) {
        return mailbox::EnqueueResult{
            .code = mailbox::EnqueueResultCode::ActorNotFound, .target = target};
    }
    mailbox->enqueue(msg.release());
    return mailbox::EnqueueResult{.code = mailbox::EnqueueResultCode::Accepted,
                                  .target = target};
}

} // namespace hpactor
