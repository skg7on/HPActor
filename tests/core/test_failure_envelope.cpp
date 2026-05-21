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

#include <hpactor/types/failure_envelope.hpp>

#include <cstdlib>
#include <cstring>
#include <string>

// Always-on assertion (NDEBUG strips standard assert in Release builds).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

int main() {
    // ── Default-constructed envelope ────────────────────────────────
    {
        hpactor::FailureEnvelope env;
        CHECK(env.reason == hpactor::FailureReason::Unknown);
        CHECK(env.actor_id == hpactor::ActorId{});
        CHECK(env.message_id == hpactor::MessageId{});
        CHECK(env.retryable == false);
        CHECK(env.timestamp_ns == 0);
        CHECK(env.detail_len == 0);
        CHECK(env.source == hpactor::FailureSource::ActorRuntime);
    }

    // ── make_failure_envelope() fills all fields ────────────────────
    {
        hpactor::ActorId target_id{42};
        hpactor::ActorAddress sender_addr;
        hpactor::ActorAddress receiver_addr;
        hpactor::MessageId msg_id{100};
        hpactor::TraceContext trace_ctx;

        auto env = hpactor::make_failure_envelope(
            hpactor::FailureReason::MailboxFull, target_id, sender_addr,
            receiver_addr, msg_id, trace_ctx, hpactor::FailureSource::Mailbox,
            "depth=1024 capacity=1024");

        CHECK(env.reason == hpactor::FailureReason::MailboxFull);
        CHECK(env.actor_id == target_id);
        CHECK(env.message_id == msg_id);
        CHECK(env.retryable == true); // MailboxFull is retryable
        CHECK(env.timestamp_ns > 0);
        CHECK(env.source == hpactor::FailureSource::Mailbox);
        CHECK(env.detail_len > 0);
        CHECK(env.detail_view() == "depth=1024 capacity=1024");
    }

    // ── Non-retryable reason ────────────────────────────────────────
    {
        hpactor::ActorId target{1};
        hpactor::ActorAddress addr;
        hpactor::MessageId mid;
        hpactor::TraceContext tc;

        auto env = hpactor::make_failure_envelope(
            hpactor::FailureReason::ActorDead, target, addr, addr, mid, tc,
            hpactor::FailureSource::ActorRuntime, "");

        CHECK(env.reason == hpactor::FailureReason::ActorDead);
        CHECK(env.retryable == false);
        CHECK(env.detail_len == 0);
    }

    // ── set_detail() truncation at 255 chars ────────────────────────
    {
        hpactor::FailureEnvelope env;
        std::string str255(255, 'y');
        env.set_detail(str255);
        CHECK(env.detail_len == 255);
        // detail is null-terminated after set_detail
        CHECK(env.detail[255] == '\0');
    }

    // ── set_detail() truncation when string exceeds array ───────────
    {
        hpactor::FailureEnvelope env;
        std::string long_str(300, 'x');
        env.set_detail(long_str);
        CHECK(env.detail_len == 255); // capped at detail.size() - 1
        CHECK(env.detail[255] == '\0');
    }

    // ── Overwrite set_detail preserves null terminator ──────────────
    {
        hpactor::FailureEnvelope env;
        env.set_detail("hello");
        CHECK(env.detail_len == 5);
        CHECK(env.detail_view() == "hello");
        env.set_detail("hi");
        CHECK(env.detail_len == 2);
        CHECK(env.detail_view() == "hi");
        CHECK(env.detail[2] == '\0'); // null terminator at new position
    }

    // ── retryable flag matches retryable(FailureReason) ─────────────
    {
        hpactor::ActorId id{1};
        hpactor::ActorAddress a;
        hpactor::MessageId m;
        hpactor::TraceContext t;

        auto env = hpactor::make_failure_envelope(
            hpactor::FailureReason::Timeout, id, a, a, m, t,
            hpactor::FailureSource::ActorRuntime, "");

        CHECK(env.retryable == hpactor::retryable(env.reason));
    }

    // ── Factory with no detail ──────────────────────────────────────
    {
        hpactor::ActorId id{7};
        hpactor::ActorAddress a;
        hpactor::MessageId m{42};
        hpactor::TraceContext t;

        auto env = hpactor::make_failure_envelope(
            hpactor::FailureReason::NoRoute, id, a, a, m, t,
            hpactor::FailureSource::ActorRuntime);

        CHECK(env.reason == hpactor::FailureReason::NoRoute);
        CHECK(env.detail_len == 0);
        CHECK(env.detail_view().empty());
    }

    return 0;
}
