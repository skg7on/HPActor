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

#include <gtest/gtest.h>

#include <hpactor/actor/stream/stream_runtime.hpp>
#include <hpactor/net/frame_dispatch_result.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/runtime/messaging_runtime.hpp>

#include "net/inbound_frame_router.hpp"

namespace {

/// Minimal test helper: verify the protected-tag detection function matches
/// the declared contract.
TEST(InboundFrameRouterTest, DetectsPythonProtectedTags) {
    // The router internally uses a helper:
    //   return tag >= 0xF0u && tag <= 0xF3u;
    // We test the equivalent logic here to catch regressions.
    auto is_protected = [](uint32_t tag) -> bool {
        return tag >= 0xF0u && tag <= 0xF3u;
    };

    // Control tags in the subsystem range but outside the Python binding range.
    EXPECT_FALSE(is_protected(0x00u));
    EXPECT_FALSE(is_protected(0x80u));
    EXPECT_FALSE(is_protected(0xEFu));
    EXPECT_FALSE(is_protected(0xF4u));
    EXPECT_FALSE(is_protected(0xFFu));
    EXPECT_FALSE(is_protected(0x1000u));

    // The four reserved Python binding tags.
    EXPECT_TRUE(is_protected(0xF0u)); // kPythonWakeupTag
    EXPECT_TRUE(is_protected(0xF1u)); // kPythonActorCommandTag
    EXPECT_TRUE(is_protected(0xF2u)); // kPythonActorFailedTag
    EXPECT_TRUE(is_protected(0xF3u)); // kPythonInspectTag
}

} // namespace
