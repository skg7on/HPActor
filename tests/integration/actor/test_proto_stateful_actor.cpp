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

#include <hpactor/actor/proto_stateful_actor.hpp>

#include <gtest/gtest.h>

struct CounterState {
    int count = 0;
};

class CounterActor : public hpactor::ProtoStatefulActor<CounterState> {
  public:
    using ProtoStatefulActor::ProtoStatefulActor;

  protected:
    void register_handlers() override {
        // Handlers will be registered once proto messages are defined
    }
};

TEST(ProtoStatefulActorTest, TypeCompilation) {
    static_assert(sizeof(CounterActor) > 0, "should not be empty");
}
