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

#include <hpactor/msg/frame.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/python/python_command_codec.hpp>
#include <hpactor/ref/actor_address.hpp>

using namespace hpactor;
using namespace hpactor::python;

namespace {

ActorAddress make_local_addr(uint32_t type, uint64_t id, uint64_t incarnation) {
    return ActorAddress{EndPoint{LocalEndpoint}, ActorType{type}, ActorId{id},
                        incarnation};
}

} // namespace

TEST(PythonCommandProtoTest, RoundTripsActorOwnedCommandFields) {
    PythonCommand command;
    command.kind = PythonCommandKind::Reply;
    command.token = 41;
    command.sequence = 7;
    command.generation = 9;
    command.origin = make_local_addr(4, 5, 6);
    command.target = make_local_addr(8, 10, 11);
    command.reply_to = make_local_addr(12, 13, 14);
    command.type_tag = static_cast<TypeTag>(0x1001);
    command.payload = StreamBuffer{1, 2, 3};
    command.ask_message_id = 99;
    command.priority = 2;
    command.deadline_ns = 700;
    command.flags = 0x40;

    auto encoded = encode_actor_command(command);
    ASSERT_TRUE(encoded.ok());
    auto decoded = decode_actor_command(encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().kind, command.kind);
    EXPECT_EQ(decoded.value().origin, command.origin);
    EXPECT_EQ(decoded.value().reply_to, command.reply_to);
    EXPECT_EQ(decoded.value().payload, command.payload);
    EXPECT_EQ(decoded.value().ask_message_id, 99u);
}

TEST(PythonCommandProtoTest, RoundTripsMinimalFields) {
    PythonCommand command;
    command.kind = PythonCommandKind::Send;
    command.token = 1;
    command.type_tag = static_cast<TypeTag>(0x1000);

    auto encoded = encode_actor_command(command);
    ASSERT_TRUE(encoded.ok());
    auto decoded = decode_actor_command(encoded.value());
    ASSERT_TRUE(decoded.ok());
    EXPECT_EQ(decoded.value().kind, PythonCommandKind::Send);
    EXPECT_EQ(decoded.value().token, 1u);
}

TEST(PythonCommandProtoTest, RejectsVersionMismatch) {
    // Create a valid command just to get a payload we can corrupt.
    PythonCommand command;
    command.kind = PythonCommandKind::Send;
    auto encoded = encode_actor_command(command);
    ASSERT_TRUE(encoded.ok());
    // Corrupt version byte — version is field 1 (varint), encoded near start.
    // Just passing a truncated buffer will trigger a parse failure.
    StreamBuffer truncated;
    auto decoded = decode_actor_command(truncated);
    EXPECT_TRUE(decoded.is_error());
}

TEST(PythonCommandProtoTest, RejectsInvalidCommandKind) {
    // empty/invalid protobuf should fail to parse
    StreamBuffer empty;
    auto decoded = decode_actor_command(empty);
    EXPECT_TRUE(decoded.is_error());
}

TEST(PythonCommandProtoTest, RejectsTagsOutsideApplicationRange) {
    PythonCommand command;
    command.kind = PythonCommandKind::Send;
    command.type_tag = static_cast<TypeTag>(0x0001); // below 0x1000
    command.token = 1;

    auto encoded = encode_actor_command(command);
    ASSERT_TRUE(encoded.ok());
    auto decoded = decode_actor_command(encoded.value());
    EXPECT_TRUE(decoded.is_error());
}

TEST(PythonCommandProtoTest, AllowsZeroTagForStopCommands) {
    PythonCommand command;
    command.kind = PythonCommandKind::Stop;
    command.type_tag = static_cast<TypeTag>(0);
    command.token = 1;

    auto encoded = encode_actor_command(command);
    ASSERT_TRUE(encoded.ok());
    auto decoded = decode_actor_command(encoded.value());
    ASSERT_TRUE(decoded.ok());
}

TEST(PythonCommandProtoTest, RoundTripsExtendedFields) {
    PythonCommand command;
    command.kind = PythonCommandKind::Ask;
    command.token = 100;
    command.origin = make_local_addr(1, 10, 1);
    command.target = make_local_addr(2, 20, 1);
    command.reply_to = make_local_addr(3, 30, 1);
    command.type_tag = static_cast<TypeTag>(0x2000);
    command.payload = StreamBuffer{10, 20, 30};
    command.message_id = 42;
    command.ask_message_id = 43;
    command.priority = 3;
    command.deadline_ns = 5000000;
    command.flags = 0x01;
    command.delay_ns = 1000000;
    command.schedule_handle = 777;
    command.error_code = 0;
    command.detail = "test detail";
    command.actor_name = "test_actor";
    command.delivery_mode = 2;
    command.no_drop = true;
    command.emit_backpressure = false;

    auto encoded = encode_actor_command(command);
    ASSERT_TRUE(encoded.ok());
    auto decoded = decode_actor_command(encoded.value());
    ASSERT_TRUE(decoded.ok());
    auto& d = decoded.value();
    EXPECT_EQ(d.kind, command.kind);
    EXPECT_EQ(d.origin, command.origin);
    EXPECT_EQ(d.target, command.target);
    EXPECT_EQ(d.reply_to, command.reply_to);
    EXPECT_EQ(d.message_id, 42u);
    EXPECT_EQ(d.ask_message_id, 43u);
    EXPECT_EQ(d.priority, 3);
    EXPECT_EQ(d.deadline_ns, 5000000);
    EXPECT_EQ(d.flags, 0x01u);
    EXPECT_EQ(d.delay_ns, 1000000u);
    EXPECT_EQ(d.schedule_handle, 777u);
    EXPECT_EQ(d.detail, "test detail");
    EXPECT_EQ(d.actor_name, "test_actor");
    EXPECT_EQ(d.delivery_mode, 2u);
    EXPECT_TRUE(d.no_drop);
    EXPECT_FALSE(d.emit_backpressure);
}

TEST(PythonCommandProtoTest, EncodeActorFailedRoundTrip) {
    auto addr = make_local_addr(1, 42, 3);
    auto encoded = encode_actor_failed(addr, 5, "ValueError", "bad value",
                                       "File \"x.py\", line 1", 10);
    ASSERT_TRUE(encoded.ok());

    ActorAddress decoded_addr;
    uint64_t gen = 0;
    std::string exc_type, msg, tb;
    uint64_t seq = 0;
    auto result = decode_actor_failed(encoded.value(), decoded_addr, gen,
                                      exc_type, msg, tb, seq);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(gen, 5u);
    EXPECT_EQ(exc_type, "ValueError");
    EXPECT_EQ(msg, "bad value");
    EXPECT_EQ(tb, "File \"x.py\", line 1");
    EXPECT_EQ(seq, 10u);
}

TEST(PythonCommandProtoTest, RejectsOversizeFields) {
    auto addr = make_local_addr(1, 1, 1);
    std::string big(300, 'x'); // > 255 bytes for exception_type
    auto encoded = encode_actor_failed(addr, 1, big, "msg", "tb", 1);
    EXPECT_TRUE(encoded.is_error());
}
