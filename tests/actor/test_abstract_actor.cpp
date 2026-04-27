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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <type_traits>

using namespace hpactor;

// Test that AbstractActor has the required interface
void test_abstract_actor_interface() {
    // Abstract actor cannot be instantiated directly
    // Test that it has the required virtual interface
    static_assert(sizeof(hpactor::AbstractActor) > 0, "AbstractActor should "
                                                      "not be empty");

    // Check that AbstractActor is abstract (has pure virtual methods)
    static_assert(!std::is_default_constructible_v<AbstractActor>, "AbstractAct"
                                                                   "or should "
                                                                   "not be "
                                                                   "default "
                                                                   "constructib"
                                                                   "le");

    // Check that it inherits from enable_shared_from_this
    static_assert(
        std::is_base_of_v<std::enable_shared_from_this<AbstractActor>, AbstractActor>,
        "AbstractActor must inherit from enable_shared_from_this");
}

void test_typed_message_types() {
    // Test that TypedMessage can identify all required system message types
    TypedMessage msg(TypeTag::DownMsg, bytes{});
    assert(msg.type_id() == TypeTag::DownMsg);

    msg = TypedMessage(TypeTag::ExitMsg, bytes{});
    assert(msg.type_id() == TypeTag::ExitMsg);

    msg = TypedMessage(TypeTag::LinkMsg, bytes{});
    assert(msg.type_id() == TypeTag::LinkMsg);

    msg = TypedMessage(TypeTag::UnlinkMsg, bytes{});
    assert(msg.type_id() == TypeTag::UnlinkMsg);
}

int main() {
    test_abstract_actor_interface();
    test_typed_message_types();

    return 0;
}
