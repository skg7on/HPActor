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
#include <hpactor/common.pb.h> // for test protobuf types
#include <hpactor/msg/typed_message.hpp>

TEST(TypedMessageAdvancedTest, FromProtobufMessage) {
    ::hpactor::PbIpv4Endpoint ep;
    ep.set_addr(0x7F000001);
    ep.set_port(8080);

    hpactor::TypedMessage msg(hpactor::TypeTag::User, ep);
    EXPECT_EQ(msg.type_id(), hpactor::TypeTag::User);
    EXPECT_FALSE(msg.payload().empty());
}

TEST(TypedMessageAdvancedTest, LazyDeserializationWithAsT) {
    ::hpactor::PbIpv4Endpoint ep;
    ep.set_addr(0x7F000001);
    ep.set_port(8080);

    hpactor::TypedMessage msg(hpactor::TypeTag::User, ep);

    auto parsed = msg.as<::hpactor::PbIpv4Endpoint>();
    ASSERT_NE(parsed, nullptr);
    EXPECT_EQ(parsed->addr(), 0x7F000001);
    EXPECT_EQ(parsed->port(), 8080);
}

TEST(TypedMessageAdvancedTest, ParsedAccessAfterAsTCachesResult) {
    ::hpactor::PbIpv4Endpoint ep;
    ep.set_addr(0x7F000001);
    ep.set_port(8080);

    hpactor::TypedMessage msg(hpactor::TypeTag::User, ep);
    auto parsed = msg.as<::hpactor::PbIpv4Endpoint>();
    (void)parsed;
    EXPECT_NE(msg.parsed(), nullptr);
}

TEST(TypedMessageAdvancedTest, MovePreservesParsedState) {
    ::hpactor::PbIpv4Endpoint ep;
    ep.set_addr(0x7F000001);
    ep.set_port(8080);

    hpactor::TypedMessage msg(hpactor::TypeTag::User, ep);
    auto parsed = msg.as<::hpactor::PbIpv4Endpoint>();
    (void)parsed;

    auto msg2 = std::move(msg);
    EXPECT_NE(msg2.parsed(), nullptr);
    EXPECT_GT(msg2.payload().size(), 0u);
}
