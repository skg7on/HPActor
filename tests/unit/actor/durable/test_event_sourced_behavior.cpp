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

#include <hpactor/actor/durable/event_sourced_behavior.hpp>
#include <hpactor/actor/durable_state_store.hpp>
#include <hpactor/types/types.hpp>

#include "durable_test_helpers.hpp"

#include <memory>
#include <string>

namespace hpactor::actor::durable {

// serialize_state/deserialize_state for TestState are shared in
// durable_test_helpers.hpp

// ---- Event-sourced specializations for TestEvent ----

template <>
result<void> apply_event_to_state(TestState& s, const TestEvent& e) {
    s.counter += e.delta;
    return result<void>::make();
}

template <> StreamBuffer serialize_event(const TestEvent& e) {
    auto data = std::to_string(e.delta);
    return StreamBuffer::from_data(reinterpret_cast<const uint8_t*>(data.c_str()),
                                   data.size());
}

template <> result<TestEvent> deserialize_event(const StreamBuffer& data) {
    std::string s(reinterpret_cast<const char*>(data.data()), data.size());
    TestEvent ev;
    ev.delta = std::stoi(s);
    return result<TestEvent>::make(std::move(ev));
}

// ---- Test fixture ----

class EventSourcedBehaviorTest : public ::testing::Test {
  protected:
    void SetUp() override {
        store_ = std::make_unique<TestInMemoryStore>();
    }
    std::unique_ptr<DurableStateStore> store_;
};

TEST_F(EventSourcedBehaviorTest, PersistEventAndRecover) {
    {
        EventSourcedBehavior<TestState, TestEvent> b1("actor-es-1", *store_,
                                                      TestState{0, "start"});
        b1.recover();
        b1.persist_event_and_apply(TestEvent{5});
        EXPECT_EQ(b1.state().counter, 5);
        b1.persist_event_and_apply(TestEvent{3});
        EXPECT_EQ(b1.state().counter, 8);
        b1.snapshot();
    }
    {
        EventSourcedBehavior<TestState, TestEvent> b2("actor-es-1", *store_,
                                                      TestState{0, "default"});
        b2.recover();
        EXPECT_TRUE(b2.is_recovered());
        EXPECT_EQ(b2.state().counter, 8);
    }
}

} // namespace hpactor::actor::durable
