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
#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/types/types.hpp>

#include <type_traits>

using namespace hpactor;

TEST(AbstractActorTest, Interface) {
    static_assert(sizeof(hpactor::AbstractActor) > 0, "AbstractActor should "
                                                      "not be empty");
    static_assert(!std::is_default_constructible_v<AbstractActor>, "AbstractAct"
                                                                   "or should "
                                                                   "not be "
                                                                   "default "
                                                                   "constructib"
                                                                   "le");
    static_assert(
        std::is_base_of_v<std::enable_shared_from_this<AbstractActor>, AbstractActor>,
        "AbstractActor must inherit from enable_shared_from_this");
}

TEST(AbstractActorTest, TypedMessageTypes) {
    TypedMessage msg(TypeTag::DownMsg, StreamBuffer{});
    EXPECT_EQ(msg.type_id(), TypeTag::DownMsg);

    msg = TypedMessage(TypeTag::ExitMsg, StreamBuffer{});
    EXPECT_EQ(msg.type_id(), TypeTag::ExitMsg);

    msg = TypedMessage(TypeTag::LinkMsg, StreamBuffer{});
    EXPECT_EQ(msg.type_id(), TypeTag::LinkMsg);

    msg = TypedMessage(TypeTag::UnlinkMsg, StreamBuffer{});
    EXPECT_EQ(msg.type_id(), TypeTag::UnlinkMsg);
}
