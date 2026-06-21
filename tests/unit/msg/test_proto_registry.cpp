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
#include <hpactor/common.pb.h>
#include <hpactor/msg/proto_type_registry.hpp>

using namespace hpactor;

TEST(ProtoTypeRegistryTest, Construction) {
    ProtoTypeRegistry reg;
    EXPECT_FALSE(reg.has_tag(TypeTag::User));
}

TEST(ProtoTypeRegistryTest, RegisterType) {
    ProtoTypeRegistry reg;
    reg.register_type<PbActorRef>(TypeTag::User, "hpactor.PbActorRef");
    EXPECT_TRUE(reg.has_tag(TypeTag::User));
    EXPECT_EQ(reg.type_name(TypeTag::User), "hpactor.PbActorRef");
}

TEST(ProtoTypeRegistryTest, CreateRegisteredType) {
    ProtoTypeRegistry reg;
    reg.register_type<PbActorRef>(TypeTag::User, "hpactor.PbActorRef");
    auto msg = reg.create(TypeTag::User);
    EXPECT_NE(msg, nullptr);
    EXPECT_EQ(msg->GetTypeName(), "hpactor.PbActorRef");
}

TEST(ProtoTypeRegistryTest, CreateUnregisteredTag) {
    ProtoTypeRegistry reg;
    auto msg = reg.create(static_cast<TypeTag>(999));
    EXPECT_EQ(msg, nullptr);
}

TEST(ProtoTypeRegistryTest, DeserializeUnregisteredTag) {
    ProtoTypeRegistry reg;
    StreamBuffer data = {0x00, 0x00, 0x00, 0x01, 0x00};
    auto msg = reg.deserialize(static_cast<TypeTag>(1), data);
    EXPECT_EQ(msg, nullptr);
}

TEST(ProtoTypeRegistryTest, WireEncodeDecodeRoundTrip) {
    ProtoTypeRegistry reg;
    reg.register_type<PbActorRef>(TypeTag::User, "hpactor.PbActorRef");

    PbActorRef ref;
    auto* local = ref.mutable_local_addr();
    local->set_actor_type(1);
    local->set_actor_id(42);
    local->set_incarnation(0);

    StreamBuffer wire = reg.encode_wire(TypeTag::User, ref);
    EXPECT_GT(wire.size(), 4u);

    auto [tag, msg] = reg.decode_wire(wire);
    EXPECT_EQ(tag, TypeTag::User);
    EXPECT_NE(msg, nullptr);
    auto* decoded = static_cast<PbActorRef*>(msg.get());
    EXPECT_NE(decoded, nullptr);
    EXPECT_EQ(decoded->local_addr().actor_type(), 1);
    EXPECT_EQ(decoded->local_addr().actor_id(), 42u);
    EXPECT_EQ(decoded->local_addr().incarnation(), 0u);
}

TEST(ProtoTypeRegistryTest, DecodeShortBuffer) {
    ProtoTypeRegistry reg;
    StreamBuffer short_buf = {0x00, 0x00};
    auto [tag, msg] = reg.decode_wire(short_buf);
    EXPECT_EQ(tag, TypeTag::Invalid);
    EXPECT_EQ(msg, nullptr);
}

TEST(ProtoTypeRegistryTest, MessageTraitsSystemType) {
    TypeTag found = MessageTraits<DownMessage>::tag();
    EXPECT_EQ(found, TypeTag::DownMsg);
}

TEST(ProtoTypeRegistryTest, MessageTraitsUnregisteredType) {
    TypeTag tag = MessageTraits<PbActorRef>::tag();
    EXPECT_EQ(tag, TypeTag::Invalid);
}
