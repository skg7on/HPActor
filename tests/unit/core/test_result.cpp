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
#include <hpactor/types/types.hpp>

TEST(ResultTest, ResultValue) {
    auto r = hpactor::result<int>::make(42);
    EXPECT_TRUE(r.has_value());
    EXPECT_EQ(r.value(), 42);
}

TEST(ResultTest, ResultError) {
    auto r = hpactor::result<int>::make(hpactor::error{1, "test"});
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), 1);
}

TEST(ResultTest, ResultVoidSuccess) {
    auto r = hpactor::result<void>::make();
    EXPECT_TRUE(r.has_value());
}

TEST(ResultTest, ResultVoidError) {
    auto r = hpactor::result<void>::make(hpactor::error{42, "specific error"});
    EXPECT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code(), 42);
}
