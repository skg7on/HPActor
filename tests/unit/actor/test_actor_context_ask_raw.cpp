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

#include <hpactor/actor/actor_context.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/types/types.hpp>

using namespace hpactor;

/// Verify that the typed ask_raw overload exists and is callable through
/// the public ActorContext API. This is a compile-and-link test — the
/// actual ask infrastructure is exercised by the integration-level
/// ask-manager tests.
TEST(ActorContextAskRawTest, TypedOverloadSignatureExists) {
    // The typed ask_raw overload is declared in actor_context.hpp.
    // Verify we can form the function pointer.
    using TypedAskRawFn = RequestHandle<StreamBuffer> (ActorContext::*)(
        const ActorAddress&, TypeTag, const StreamBuffer&, RequestTimeout);
    // If this compiles, the overload exists.
    TypedAskRawFn fn = &ActorContext::ask_raw;
    (void)fn;
    SUCCEED() << "Typed ask_raw overload is declared and addressable";
}
