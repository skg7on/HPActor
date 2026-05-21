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

#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <hpactor/types/types.hpp>

#include <cstdlib>
#include <cstring>

// Always-on assertion (NDEBUG strips standard assert in Release builds).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

int main() {
    // ── retryable() ─────────────────────────────────────────────────
    CHECK(hpactor::retryable(hpactor::FailureReason::NoRoute));
    CHECK(hpactor::retryable(hpactor::FailureReason::NodeUnavailable));
    CHECK(!hpactor::retryable(hpactor::FailureReason::ActorDead));
    CHECK(hpactor::retryable(hpactor::FailureReason::ActorNotReady));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Quarantined));
    CHECK(hpactor::retryable(hpactor::FailureReason::CircuitOpen));
    CHECK(hpactor::retryable(hpactor::FailureReason::MailboxFull));
    CHECK(hpactor::retryable(hpactor::FailureReason::OutboundQueueFull));
    CHECK(hpactor::retryable(hpactor::FailureReason::MemoryPressure));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Expired));
    CHECK(hpactor::retryable(hpactor::FailureReason::Timeout));
    CHECK(!hpactor::retryable(hpactor::FailureReason::RejectedByPolicy));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Dropped));
    CHECK(!hpactor::retryable(hpactor::FailureReason::MailboxClosed));
    CHECK(!hpactor::retryable(hpactor::FailureReason::SerializationError));
    CHECK(hpactor::retryable(hpactor::FailureReason::TransportError));
    CHECK(!hpactor::retryable(hpactor::FailureReason::FrameRejected));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Duplicate));
    CHECK(hpactor::retryable(hpactor::FailureReason::Draining));
    CHECK(hpactor::retryable(hpactor::FailureReason::ShuttingDown));
    CHECK(!hpactor::retryable(hpactor::FailureReason::RetryExhausted));
    CHECK(!hpactor::retryable(hpactor::FailureReason::SpawnFailed));
    CHECK(!hpactor::retryable(hpactor::FailureReason::Unknown));

    // ── to_string(FailureReason) ────────────────────────────────────
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::NoRoute), "no_"
                                                                           "rou"
                                                                           "t"
                                                                           "e") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::ActorDead),
                      "actor_dead") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::MailboxFull),
                      "mailbox_full") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::Timeout), "tim"
                                                                           "eou"
                                                                           "t") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureReason::Unknown), "unk"
                                                                           "now"
                                                                           "n") == 0);

    // to_string returns non-null for all enum values up to 90
    for (uint8_t i = 0; i <= 90; ++i) {
        auto r = static_cast<hpactor::FailureReason>(i);
        const char* s = hpactor::to_string(r);
        CHECK(s != nullptr);
        CHECK(std::strlen(s) > 0);
    }

    // ── to_string(FailureSource) ────────────────────────────────────
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureSource::ActorRuntime),
                      "actor_runtime") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureSource::Mailbox), "mai"
                                                                           "lbo"
                                                                           "x") == 0);
    CHECK(std::strcmp(hpactor::to_string(hpactor::FailureSource::Unknown), "unk"
                                                                           "now"
                                                                           "n") == 0);

    // ── EnqueueResultCode → FailureReason mapping ───────────────────
    using namespace hpactor::mailbox;
    CHECK(failure_reason(EnqueueResultCode::Accepted) ==
          hpactor::FailureReason::Unknown);
    CHECK(failure_reason(EnqueueResultCode::AcceptedWithSoftPressure) ==
          hpactor::FailureReason::Unknown);
    CHECK(failure_reason(EnqueueResultCode::Rejected) ==
          hpactor::FailureReason::RejectedByPolicy);
    CHECK(failure_reason(EnqueueResultCode::DroppedNewest) ==
          hpactor::FailureReason::Dropped);
    CHECK(failure_reason(EnqueueResultCode::DroppedExisting) ==
          hpactor::FailureReason::Dropped);
    CHECK(failure_reason(EnqueueResultCode::ReroutedToDeadLetter) ==
          hpactor::FailureReason::RejectedByPolicy);
    CHECK(failure_reason(EnqueueResultCode::ReroutedToOverflow) ==
          hpactor::FailureReason::RejectedByPolicy);
    CHECK(failure_reason(EnqueueResultCode::MailboxClosed) ==
          hpactor::FailureReason::MailboxClosed);
    CHECK(failure_reason(EnqueueResultCode::ActorNotFound) ==
          hpactor::FailureReason::NoRoute);

    // ── EnqueueResult::failure_reason() method ──────────────────────
    {
        EnqueueResult r;
        r.code = EnqueueResultCode::ActorNotFound;
        CHECK(r.failure_reason() == hpactor::FailureReason::NoRoute);
    }
    {
        EnqueueResult r;
        r.code = EnqueueResultCode::Accepted;
        CHECK(r.failure_reason() == hpactor::FailureReason::Unknown);
    }

    // ── error::failure_reason() mapping ─────────────────────────────
    {
        hpactor::error err(hpactor::errors::actor_down, "down");
        CHECK(err.failure_reason() == hpactor::FailureReason::ActorDead);
    }
    {
        hpactor::error err(hpactor::errors::actor_not_found, "missing");
        CHECK(err.failure_reason() == hpactor::FailureReason::NoRoute);
    }
    {
        hpactor::error err(hpactor::errors::mailbox_full, "full");
        CHECK(err.failure_reason() == hpactor::FailureReason::MailboxFull);
    }
    {
        hpactor::error err(hpactor::errors::timeout, "timeout");
        CHECK(err.failure_reason() == hpactor::FailureReason::Timeout);
    }
    {
        hpactor::error err(hpactor::errors::invalid_argument, "bad arg");
        CHECK(err.failure_reason() == hpactor::FailureReason::RejectedByPolicy);
    }
    {
        hpactor::error err(hpactor::errors::unknown, "?");
        CHECK(err.failure_reason() == hpactor::FailureReason::Unknown);
    }
    {
        hpactor::error err(9999, "unmapped");
        CHECK(err.failure_reason() == hpactor::FailureReason::Unknown);
    }

    return 0;
}
