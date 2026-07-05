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
#include <hpactor/actor/stream/stream_config.hpp>
#include <hpactor/actor/stream/stream_sender_actor.hpp>

using namespace hpactor;

TEST(StreamSenderActorTest, StreamConfigDefaultValues) {
    StreamConfig cfg;
    EXPECT_EQ(cfg.initial_window_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.max_chunk_bytes, 64u * 1024u);
    EXPECT_EQ(cfg.send_buffer_bytes, 256u * 1024u);
    EXPECT_EQ(cfg.max_in_flight_frames, 256u);
}
