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

#include <cstdint>
#include <cstdlib>
#include <hpactor/types/types.hpp>

// Always-on assertion (NDEBUG strips standard assert in Release builds).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

int main() {
    // Test 1: ActorId default construction (value == 0)
    hpactor::ActorId default_actor_id;
    CHECK(default_actor_id.value() == 0);

    // Test 2: ActorId explicit construction from counter_type
    hpactor::ActorId explicit_actor_id(42);
    CHECK(explicit_actor_id.value() == 42);

    // Test 3: ActorId equality
    hpactor::ActorId actor_id1(100);
    hpactor::ActorId actor_id2(100);
    hpactor::ActorId actor_id3(200);
    CHECK(actor_id1 == actor_id2);

    // Test 4: ActorId inequality
    CHECK(actor_id1 != actor_id3);

    // Test 5: ActorId value accessor
    CHECK(actor_id1.value() == 100);

    // Test 6: LocalEndpoint
    CHECK(hpactor::LocalEndpoint.is_loopback());
    CHECK(hpactor::LocalEndpoint.port() == 0);

    // Test 7: ActorType with InvalidActorType
    hpactor::ActorType actor_type = hpactor::InvalidActorType;
    CHECK(actor_type == hpactor::InvalidActorType);

    // Test 8: error class
    hpactor::error ok_err;
    CHECK(ok_err.ok());
    CHECK(!ok_err);

    hpactor::error err(42, "test error");
    CHECK(!err.ok());
    CHECK(err);
    CHECK(err.code() == 42);
    CHECK(err.message() == "test error");

    // Test 9: errors namespace
    CHECK(hpactor::errors::unknown == 1);
    CHECK(hpactor::errors::actor_down == 2);
    CHECK(hpactor::errors::actor_not_found == 3);
    CHECK(hpactor::errors::mailbox_full == 4);
    CHECK(hpactor::errors::timeout == 5);
    CHECK(hpactor::errors::user == 1000);

    // Test 10: MessageId generate
    hpactor::MessageId id1 = hpactor::generate_message_id();
    hpactor::MessageId id2 = hpactor::generate_message_id();
    CHECK(id1 != id2); // Each call should be unique

    // Test 11: Clock
    hpactor::Clock clock;
    hpactor::Clock::time_point tp = clock.now();
    hpactor::Clock::duration dur = hpactor::Clock::duration(100);
    hpactor::Clock::time_point tp2 = tp + dur;
    CHECK(tp2 > tp);

    // Test 12: AlarmHandle
    hpactor::AlarmHandle handle1;
    hpactor::AlarmHandle handle2(42);
    CHECK(handle1.value() == 0);
    CHECK(handle2.value() == 42);

    // Test 13: TraceContext
    hpactor::TraceContext ctx;
    ctx.trace_id.bytes[15] = 1;
    ctx.span_id.bytes[7] = 2;
    ctx.flags.value = 3;
    CHECK(ctx.trace_id.valid());
    CHECK(ctx.span_id.valid());
    CHECK(ctx.flags.value == 3);

    // Test 14: StreamBuffer
    hpactor::StreamBuffer data = {1, 2, 3, 4, 5};
    CHECK(data.size() == 5);
    CHECK(data[0] == 1);
    CHECK(data[4] == 5);

    return 0;
}