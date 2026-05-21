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

#include <cstdlib>
#include <cstring>
#include <hpactor/spawn.hpp>
#include <hpactor/types/failure_reason.hpp>
#include <iostream>

// Always-on assertion (NDEBUG strips standard assert in Release builds).
// NOLINTNEXTLINE(cppcoreguidelines-macro-usage)
#define CHECK(expr)                                                            \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::abort();                                                      \
        }                                                                      \
    } while (false)

int main() {
    using namespace hpactor;

    // spawn_errors -> FailureReason mapping
    CHECK(failure_reason(spawn_errors::success) == FailureReason::Unknown);
    CHECK(failure_reason(spawn_errors::unknown_type) == FailureReason::NoRoute);
    CHECK(failure_reason(spawn_errors::deserialization_failed) ==
          FailureReason::SerializationError);
    CHECK(failure_reason(spawn_errors::node_unreachable) ==
          FailureReason::NodeUnavailable);
    CHECK(failure_reason(spawn_errors::timeout) == FailureReason::Timeout);
    CHECK(failure_reason(spawn_errors::spawn_receiver_not_running) ==
          FailureReason::ActorNotReady);

    // Unknown spawn code maps to SpawnFailed
    CHECK(failure_reason(99) == FailureReason::SpawnFailed);

    // Verify retryable property for spawn-relevant reasons
    CHECK(retryable(FailureReason::NoRoute));
    CHECK(retryable(FailureReason::NodeUnavailable));
    CHECK(retryable(FailureReason::ActorNotReady));
    CHECK(retryable(FailureReason::Timeout));
    CHECK(!retryable(FailureReason::SerializationError));
    CHECK(!retryable(FailureReason::SpawnFailed));

    std::cout << "PASS: all spawn failure reason mapping tests\n";
    return 0;
}
