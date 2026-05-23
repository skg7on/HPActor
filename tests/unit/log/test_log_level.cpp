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

#include <cstring>
#include <gtest/gtest.h>
#include <hpactor/log/log_level.hpp>

using namespace hpactor::log;

TEST(LogLevelTest, ToString) {
    EXPECT_STREQ(to_string(LogLevel::kCritical), "critical");
    EXPECT_STREQ(to_string(LogLevel::kError), "error");
    EXPECT_STREQ(to_string(LogLevel::kWarning), "warning");
    EXPECT_STREQ(to_string(LogLevel::kInfo), "info");
    EXPECT_STREQ(to_string(LogLevel::kDebug), "debug");
    EXPECT_STREQ(to_string(LogLevel::kTrace), "trace");
    EXPECT_STREQ(to_string(LogLevel::kOff), "off");
}

TEST(LogLevelTest, ParseLevelSuccess) {
    EXPECT_TRUE(parse_level("critical").has_value());
    EXPECT_EQ(parse_level("critical").value(), LogLevel::kCritical);
    EXPECT_EQ(parse_level("error").value(), LogLevel::kError);
    EXPECT_EQ(parse_level("warning").value(), LogLevel::kWarning);
    EXPECT_EQ(parse_level("info").value(), LogLevel::kInfo);
    EXPECT_EQ(parse_level("debug").value(), LogLevel::kDebug);
    EXPECT_EQ(parse_level("trace").value(), LogLevel::kTrace);
    EXPECT_EQ(parse_level("off").value(), LogLevel::kOff);
}

TEST(LogLevelTest, ParseLevelFailure) {
    EXPECT_FALSE(parse_level("invalid").has_value());
}

TEST(LogLevelTest, Ordering) {
    EXPECT_LT(static_cast<uint8_t>(LogLevel::kCritical),
              static_cast<uint8_t>(LogLevel::kError));
    EXPECT_LT(static_cast<uint8_t>(LogLevel::kDebug),
              static_cast<uint8_t>(LogLevel::kTrace));
}

TEST(LogLevelTest, EnabledDisabledRelationships) {
    EXPECT_LE(static_cast<uint8_t>(LogLevel::kInfo),
              static_cast<uint8_t>(LogLevel::kDebug));
    EXPECT_GT(static_cast<uint8_t>(LogLevel::kDebug),
              static_cast<uint8_t>(LogLevel::kInfo));
}
