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

#include <hpactor/net/async_io_fwd.hpp>
#include <hpactor/net/reactor_backend.hpp>

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(AsyncIoBackendTest, OpTypeEnumValues) {
    EXPECT_EQ(static_cast<uint32_t>(OpType::Send), 1u);
    EXPECT_EQ(static_cast<uint32_t>(OpType::Recv), 2u);
    EXPECT_EQ(static_cast<uint32_t>(OpType::Accept), 3u);
    EXPECT_EQ(static_cast<uint32_t>(OpType::Connect), 4u);
    EXPECT_EQ(static_cast<uint32_t>(OpType::TimerFired), 5u);
    EXPECT_EQ(static_cast<uint32_t>(OpType::RecvFrom), 6u);
    EXPECT_EQ(static_cast<uint32_t>(OpType::SendTo), 7u);
}

TEST(AsyncIoBackendTest, IoEventFlagCombination) {
    IoEvent combined = static_cast<IoEvent>(static_cast<uint32_t>(IoEvent::Read) |
                                            static_cast<uint32_t>(IoEvent::Write));
    EXPECT_NE(static_cast<uint32_t>(combined) & static_cast<uint32_t>(IoEvent::Read),
              0u);
    EXPECT_NE(static_cast<uint32_t>(combined) & static_cast<uint32_t>(IoEvent::Write),
              0u);
    EXPECT_EQ(static_cast<uint32_t>(combined) &
                  static_cast<uint32_t>(static_cast<IoEvent>(0)),
              0u);
}

TEST(AsyncIoBackendTest, OpCompletionStructFields) {
    OpCompletion op;
    op.actor = ActorId(42);
    op.type = OpType::Send;
    op.fd = 7;
    op.result = 123;
    op.user_data = 999;
    EXPECT_EQ(op.actor, ActorId(42));
    EXPECT_EQ(op.type, OpType::Send);
    EXPECT_EQ(op.fd, 7);
    EXPECT_EQ(op.result, 123);
    EXPECT_EQ(op.user_data, 999u);
}

TEST(AsyncIoBackendTest, EncodeDecodeUserDataRoundtrip) {
    int fd = 5;
    ActorId actor(12345);
    uint32_t op_type = static_cast<uint32_t>(OpType::Recv);

    uint64_t encoded = encode_user_data(fd, actor, op_type);
    int fd_out;
    ActorId actor_out;
    uint32_t op_type_out;
    decode_user_data(encoded, fd_out, actor_out, op_type_out);

    EXPECT_EQ(fd_out, fd);
    EXPECT_EQ(actor_out, actor);
    EXPECT_EQ(op_type_out, op_type);
}
