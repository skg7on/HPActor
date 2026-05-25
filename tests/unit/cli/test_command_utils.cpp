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

#include "command_utils.hpp"

#include <gtest/gtest.h>
#include <limits>

using namespace hpactor::cli;
using hpactor::ActorId;

TEST(ParseActorId, ParseDecimal) {
    EXPECT_EQ(parse_actor_id("123"), ActorId(123));
    EXPECT_EQ(parse_actor_id("0"), ActorId(0));
    EXPECT_EQ(parse_actor_id("42"), ActorId(42));
}

TEST(ParseActorId, ParseHexLower) {
    EXPECT_EQ(parse_actor_id("0xABCD"), ActorId(0xABCD));
    EXPECT_EQ(parse_actor_id("0xff"), ActorId(0xFF));
}

TEST(ParseActorId, ParseHexUpper) {
    EXPECT_EQ(parse_actor_id("0XDEAD"), ActorId(0xDEAD));
    EXPECT_EQ(parse_actor_id("0XBEEF"), ActorId(0xBEEF));
}

TEST(ParseActorId, ParseHexMixedCase) {
    EXPECT_EQ(parse_actor_id("0xAbC"), ActorId(0xABC));
    EXPECT_EQ(parse_actor_id("0XaBc"), ActorId(0xABC));
}

TEST(ParseActorId, ParseMaxValue) {
    EXPECT_EQ(parse_actor_id("18446744073709551615"), ActorId(UINT64_MAX));
    EXPECT_EQ(parse_actor_id("0xFFFFFFFFFFFFFFFF"), ActorId(UINT64_MAX));
}

TEST(ParseActorId, ParseEmptyStringReturnsZero) {
    EXPECT_EQ(parse_actor_id(""), ActorId{0});
}

TEST(ParseActorId, ParseNonNumericReturnsZero) {
    EXPECT_EQ(parse_actor_id("garbage"), ActorId{0});
    EXPECT_EQ(parse_actor_id("abc"), ActorId{0});
}

TEST(ParseActorId, ParsePartialHexReturnsZero) {
    EXPECT_EQ(parse_actor_id("0xGHIJ"), ActorId{0});
    EXPECT_EQ(parse_actor_id("0xZZZ"), ActorId{0});
}

TEST(ParseActorId, ParseLeadingWhitespaceReturnsZero) {
    EXPECT_EQ(parse_actor_id(" 123"), ActorId{0});
    EXPECT_EQ(parse_actor_id("\t0xABC"), ActorId{0});
}

TEST(ParseActorId, ParseStopsAtTrailingNonDigit) {
    // from_chars stops at first non-matching char; does not require full consumption
    EXPECT_EQ(parse_actor_id("123 "), ActorId(123));
    EXPECT_EQ(parse_actor_id("0xABCx"), ActorId(0xABC));
}

TEST(ParseActorId, ParseOnlyPrefixReturnsZero) {
    EXPECT_EQ(parse_actor_id("0x"), ActorId{0});
    EXPECT_EQ(parse_actor_id("0X"), ActorId{0});
}

TEST(ParseActorId, ParseSingleChar) {
    EXPECT_EQ(parse_actor_id("5"), ActorId(5));
    EXPECT_EQ(parse_actor_id("9"), ActorId(9));
}
