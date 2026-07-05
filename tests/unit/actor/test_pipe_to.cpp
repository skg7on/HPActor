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
#include <hpactor/actor/request/pipe_to.hpp>

namespace hpactor {

TEST(PipeToTest, SuccessRoutesToOnSuccess) {
    auto r = result<int>::make(42);
    ActorAddress target;

    bool success_called = false;
    bool error_called = false;

    pipe_to(
        r, target,
        [&](const ActorAddress&, int val) {
            success_called = true;
            EXPECT_EQ(val, 42);
        },
        [&](const ActorAddress&, error) { error_called = true; });

    EXPECT_TRUE(success_called);
    EXPECT_FALSE(error_called);
}

TEST(PipeToTest, ErrorRoutesToOnError) {
    auto r = result<int>::make(error(1));
    ActorAddress target;

    bool success_called = false;
    bool error_called = false;

    pipe_to(
        r, target, [&](const ActorAddress&, int) { success_called = true; },
        [&](const ActorAddress&, error e) {
            error_called = true;
            EXPECT_EQ(e.code(), 1);
        });

    EXPECT_FALSE(success_called);
    EXPECT_TRUE(error_called);
}

TEST(PipeToTest, TargetAddressPassedToCallbacks) {
    auto r = result<int>::make(7);
    ActorAddress target;
    target.id = ActorId(99);

    pipe_to(
        r, target,
        [](const ActorAddress& t, int) { EXPECT_EQ(t.id.value(), 99); },
        [](const ActorAddress&, error) { FAIL() << "should not call on_error"; });
}

TEST(PipeToTest, StringTypeCompilesAndWorks) {
    auto r = result<std::string>::make(std::string("hello"));
    ActorAddress target;

    pipe_to(
        r, target,
        [](const ActorAddress&, std::string val) { EXPECT_EQ(val, "hello"); },
        [](const ActorAddress&, error) { FAIL() << "should not call on_error"; });
}

TEST(PipeToTest, VoidResultCompilesAndWorks) {
    auto r = result<void>::make();
    ActorAddress target;
    bool called = false;

    pipe_to(
        r, target, [&](const ActorAddress&) { called = true; },
        [&](const ActorAddress&, error) { FAIL() << "should not call on_error"; });

    EXPECT_TRUE(called);
}

TEST(PipeToTest, VoidResultErrorRoutesToOnError) {
    auto r = result<void>::make(error(2));
    ActorAddress target;
    bool error_called = false;

    pipe_to(
        r, target,
        [](const ActorAddress&) { FAIL() << "should not call on_success"; },
        [&](const ActorAddress&, error e) {
            error_called = true;
            EXPECT_EQ(e.code(), 2);
        });

    EXPECT_TRUE(error_called);
}

} // namespace hpactor
