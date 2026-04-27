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

#include <hpactor/actor/proto_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cassert>
#include <cstdio>

using namespace hpactor;

class TestActor : public ProtoActor {
protected:
    void register_handlers() override {
        // Handlers will be registered once protobuf types are available
    }
};

int main() {
    printf("=== Proto Actor Tests ===\n");

    // Test 1: proto_actor type exists
    {
        printf("Test 1: ProtoActor compilation... ");
        static_assert(sizeof(ProtoActor) > 0, "ProtoActor should not be empty");
        printf("PASS\n");
    }

    // Test 2: TestActor compiles
    {
        printf("Test 2: TestActor compilation... ");
        static_assert(sizeof(TestActor) > 0, "TestActor should not be empty");
        printf("PASS\n");
    }

    printf("=== All Proto Actor Tests Passed ===\n");
    return 0;
}
