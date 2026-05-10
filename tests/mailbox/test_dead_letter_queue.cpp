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

#include <hpactor/mailbox/dead_letter_queue.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::mailbox;

int main() {
    DeadLetterConfig cfg;
    cfg.capacity = 2;
    cfg.max_payload_sample_bytes = 3;
    cfg.overflow_policy = DeadLetterOverflowPolicy::DropOldestRecord;

    DeadLetterQueue q(cfg);

    DeadLetterRecord a;
    a.reason = DeadLetterReason::ActorNotFound;
    a.source = DeadLetterSource::LocalDelivery;
    a.message_id = 1;
    a.payload_sample = StreamBuffer{1, 2, 3, 4, 5};
    assert(q.try_push(std::move(a)));

    DeadLetterRecord b;
    b.reason = DeadLetterReason::MissingRoute;
    b.source = DeadLetterSource::ActorProxy;
    b.message_id = 2;
    assert(q.try_push(std::move(b)));

    DeadLetterRecord c;
    c.reason = DeadLetterReason::NetworkPartition;
    c.source = DeadLetterSource::Transport;
    c.message_id = 3;
    assert(q.try_push(std::move(c)));

    auto snap = q.snapshot();
    assert(snap.depth == 2);
    assert(snap.total_pushed == 3);
    assert(snap.total_lost == 1);

    DeadLetterRecord out;
    assert(q.try_pop(out));
    assert(out.message_id == 2);
    assert(q.try_pop(out));
    assert(out.message_id == 3);
    assert(!q.try_pop(out));

    return 0;
}
