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

#include <hpactor/core/actor_system.hpp>
#include <hpactor/mailbox/dead_letter_queue.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    Config cfg;
    cfg.endpoint = endpoint_ops::parse_endpoint("127.0.0.1:0");
    cfg.enable_network = false;

    ActorSystem system(cfg);

    // Create a proxy to a remote actor (no network, so transport will be null)
    ActorAddress remote{endpoint_ops::parse_endpoint("10.0.0.1:9000"),
                        ActorType{1}, ActorId{44}, 0};
    ActorProxy proxy(remote, &system);

    auto result =
        proxy.try_send(remote, TypedMessage(TypeTag::User, StreamBuffer{9}));
    assert(!result.accepted());

    // Verify dead letter was captured with RemoteNodeUnreachable
    mailbox::DeadLetterRecord dl;
    assert(system.pop_dead_letter(dl));
    assert(dl.reason == mailbox::DeadLetterReason::RemoteNodeUnreachable);
    assert(dl.source == mailbox::DeadLetterSource::ActorProxy);
    assert(dl.target.id == ActorId{44});

    // No more dead letters
    assert(!system.pop_dead_letter(dl));

    return 0;
}
