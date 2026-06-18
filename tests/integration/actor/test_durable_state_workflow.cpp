// Copyright 2026 HPActor Contributors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <hpactor/actor/durable/durable_actor.hpp>
#include <hpactor/actor/durable/file_state_store.hpp>
#include <hpactor/actor/durable/in_memory_state_store.hpp>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

using namespace hpactor;

class DurableStateTest : public ::testing::Test {
  protected:
    void SetUp() override {
        temp_dir_ = std::filesystem::temp_directory_path() / "hpactor_dur_test";
        std::filesystem::create_directories(temp_dir_);
    }

    void TearDown() override {
        std::error_code ec;
        std::filesystem::remove_all(temp_dir_, ec);
    }

    std::filesystem::path temp_dir_;
};

// Helper: create test payload
StreamBuffer make_payload(const std::string& s) {
    return StreamBuffer(s.begin(), s.end());
}

TEST_F(DurableStateTest, InMemoryStoreSnapshotWriteAndLoad) {
    InMemoryStateStore store;

    auto write_res =
        store.write_snapshot("actor-1", 1, make_payload("snapshot-data"));
    ASSERT_TRUE(write_res.has_value());

    auto load_res = store.load_latest_snapshot("actor-1");
    ASSERT_TRUE(load_res.has_value());
    EXPECT_EQ(load_res.value().data, make_payload("snapshot-data"));
}

TEST_F(DurableStateTest, InMemoryStoreSnapshotOverwrite) {
    InMemoryStateStore store;
    auto res1 = store.write_snapshot("actor-1", 1, make_payload("v1"));
    ASSERT_TRUE(res1.has_value());
    auto res2 = store.write_snapshot("actor-1", 1, make_payload("v2"));
    ASSERT_TRUE(res2.has_value());

    auto load_res = store.load_latest_snapshot("actor-1");
    ASSERT_TRUE(load_res.has_value());
    EXPECT_EQ(load_res.value().data, make_payload("v2"));
}

TEST_F(DurableStateTest, InMemoryStoreEventAppendAndLoad) {
    InMemoryStateStore store;
    store.write_snapshot("actor-1", 1, make_payload("base"));

    auto ev1 = store.append_event("actor-1", 1, make_payload("ev1"));
    ASSERT_TRUE(ev1.has_value());
    auto ev2 = store.append_event("actor-1", 2, make_payload("ev2"));
    ASSERT_TRUE(ev2.has_value());
    auto ev3 = store.append_event("actor-1", 3, make_payload("ev3"));
    ASSERT_TRUE(ev3.has_value());

    // load_events_after returns events with sequence > after_sequence.
    // after_sequence=1 gives events with sequence > 1 (i.e. 2 and 3).
    auto events_res = store.load_events_after("actor-1", 1);
    ASSERT_TRUE(events_res.has_value());
    EXPECT_EQ(events_res.value().size(), 2u);
    EXPECT_EQ(events_res.value()[0].event_data, make_payload("ev2"));
    EXPECT_EQ(events_res.value()[1].event_data, make_payload("ev3"));
}

TEST_F(DurableStateTest, InMemoryStoreDelete) {
    InMemoryStateStore store;
    store.write_snapshot("actor-1", 1, make_payload("data"));
    store.append_event("actor-1", 1, make_payload("ev1"));

    auto del_res = store.delete_state("actor-1");
    ASSERT_TRUE(del_res.has_value());

    auto snap = store.load_latest_snapshot("actor-1");
    EXPECT_TRUE(snap.is_error());

    auto events = store.load_events_after("actor-1", 0);
    ASSERT_TRUE(events.has_value());
    EXPECT_TRUE(events.value().empty());

    // Delete is idempotent
    auto del_res2 = store.delete_state("actor-1");
    EXPECT_TRUE(del_res2.has_value());
}

} // namespace
