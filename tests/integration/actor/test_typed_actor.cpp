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

#include <hpactor/actor/typed_actor.hpp>
#include <hpactor/typed_behavior.hpp>

#include <gtest/gtest.h>
#include <type_traits>

// Message struct definitions for typed actor
struct add_message {
    int a;
    int b;
};

struct subtract_message {
    int a;
    int b;
};

struct shutdown_message {};

// Typed actor using message struct signatures
using calculator_actor =
    hpactor::TypedEventBasedActorRef<hpactor::result<int>(add_message),
                                     hpactor::result<int>(subtract_message),
                                     hpactor::result<void>(shutdown_message)>;

TEST(TypedActorTest, Definition) {
    static_assert(sizeof(calculator_actor) > 0, "TypedEventBasedActorRef "
                                                "should be instantiable");
}

TEST(TypedActorTest, HandlerType) {
    using handler = hpactor::handler_type<hpactor::result<int>(add_message)>;
    static_assert(std::is_same_v<typename handler::result, int>, "result type "
                                                                 "should be "
                                                                 "int");
}

TEST(TypedActorTest, TypedBehavior) {
    hpactor::TypedBehavior<hpactor::result<int>(add_message)> bh;
    (void)bh;
    SUCCEED();
}
