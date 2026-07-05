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
#include <hpactor/actor/durable/durable_state_store.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>

using namespace hpactor;

class DurableStateStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        store_ = std::make_unique<InMemoryStateStore>();
    }

    std::unique_ptr<DurableStateStore> store_;
};

TEST_F(DurableStateStoreTest, WriteAndLoadSnapshot) {
    StreamBuffer data{1, 2, 3, 4};
    auto write_result = store_->write_snapshot("actor-1", 1, data);
    ASSERT_TRUE(write_result.ok());
    EXPECT_EQ(write_result.value().persistence_id, "actor-1");
    EXPECT_EQ(write_result.value().sequence, 0u);
    EXPECT_EQ(write_result.value().schema_version, 1u);

    auto load_result = store_->load_latest_snapshot("actor-1");
    ASSERT_TRUE(load_result.ok());
    EXPECT_EQ(load_result.value().data.size(), 4u);
    EXPECT_EQ(load_result.value().data[0], 1);
    EXPECT_EQ(load_result.value().data[3], 4);
}

TEST_F(DurableStateStoreTest, LoadNonexistentReturnsError) {
    auto result = store_->load_latest_snapshot("nonexistent");
    EXPECT_FALSE(result.ok());
}

TEST_F(DurableStateStoreTest, MultipleSnapshotsReturnLatest) {
    StreamBuffer data1{1};
    StreamBuffer data2{2, 2};
    store_->write_snapshot("actor-1", 1, data1);
    store_->write_snapshot("actor-1", 1, data2);

    auto result = store_->load_latest_snapshot("actor-1");
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value().sequence, 1u);
    EXPECT_EQ(result.value().data.size(), 2u);
}

TEST_F(DurableStateStoreTest, AppendAndLoadEvents) {
    StreamBuffer ev1{10};
    StreamBuffer ev2{20};

    auto r1 = store_->append_event("actor-1", 0, ev1);
    EXPECT_TRUE(r1.ok());

    auto r2 = store_->append_event("actor-1", 1, ev2);
    EXPECT_TRUE(r2.ok());

    auto events = store_->load_events_after("actor-1", 0);
    ASSERT_TRUE(events.ok());
    EXPECT_EQ(events.value().size(), 1u);
    EXPECT_EQ(events.value()[0].sequence, 1u);
}

TEST_F(DurableStateStoreTest, DeleteState) {
    StreamBuffer data{1, 2, 3};
    store_->write_snapshot("actor-1", 1, data);

    auto del_result = store_->delete_state("actor-1");
    EXPECT_TRUE(del_result.ok());

    auto load_result = store_->load_latest_snapshot("actor-1");
    EXPECT_FALSE(load_result.ok());
}

TEST_F(DurableStateStoreTest, StoreTypeIsInMemory) {
    EXPECT_EQ(store_->store_type(), "in_memory");
}

TEST_F(DurableStateStoreTest, IndependentActors) {
    store_->write_snapshot("actor-A", 1, StreamBuffer{1});
    store_->write_snapshot("actor-B", 2, StreamBuffer{2, 2});

    auto a = store_->load_latest_snapshot("actor-A");
    auto b = store_->load_latest_snapshot("actor-B");
    ASSERT_TRUE(a.ok());
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(a.value().persistence_id, "actor-A");
    EXPECT_EQ(b.value().persistence_id, "actor-B");
    EXPECT_EQ(b.value().schema_version, 2u);
}
