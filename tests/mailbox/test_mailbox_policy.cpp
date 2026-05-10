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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::mailbox;

int main() {
    MailboxConfig cfg;
    assert(cfg.capacity.max_messages == 1024);
    assert(cfg.capacity.max_bytes == 0);
    assert(cfg.overflow_policy == OverflowPolicy::RejectNewest);
    assert(cfg.high_watermark == 0.80);
    assert(cfg.low_watermark == 0.50);
    assert(cfg.protected_system_messages == 32);
    assert(cfg.max_overflow_depth == 0);
    assert(cfg.signal_min_interval_ms == 100);
    assert(cfg.priority_aware == false);
    assert(cfg.enable_dead_letters == true);
    assert(cfg.backpressure_mode == BackpressureMode::LocalAndRemoteSignal);

    EnqueueResult accepted;
    accepted.code = EnqueueResultCode::Accepted;
    assert(accepted.accepted());
    assert(!accepted.retryable());

    EnqueueResult soft;
    soft.code = EnqueueResultCode::AcceptedWithSoftPressure;
    assert(soft.accepted());

    EnqueueResult rejected;
    rejected.code = EnqueueResultCode::Rejected;
    rejected.retry_after = std::chrono::milliseconds{5};
    assert(!rejected.accepted());
    assert(rejected.retryable());

    TypedMessage user_msg(TypeTag::User, StreamBuffer{1, 2, 3, 4});
    assert(estimate_message_bytes(user_msg) >= sizeof(TypedMessage) + 4);
    assert(is_system_message(TypeTag::DownMsg));
    assert(!is_system_message(TypeTag::User));

    return 0;
}
