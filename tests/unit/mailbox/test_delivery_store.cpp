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

#include <hpactor/mailbox/file_delivery_store.hpp>
#include <hpactor/mailbox/in_memory_delivery_store.hpp>

#include <filesystem>

namespace hpactor::mailbox {

// ===========================================================================
// InMemoryDeliveryStore tests
// ===========================================================================

class InMemoryDeliveryStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        store_ = std::make_unique<InMemoryDeliveryStore>();
    }
    std::unique_ptr<InMemoryDeliveryStore> store_;
};

TEST_F(InMemoryDeliveryStoreTest, PutAndLoadOutbox) {
    auto send = msg::PendingSend{MessageId{42}};
    auto result = store_->put_outbox(send);
    EXPECT_TRUE(result.ok());
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().size(), 1u);
}

TEST_F(InMemoryDeliveryStoreTest, MarkOutboxCompleteRemovesEntry) {
    auto send = msg::PendingSend{MessageId{42}};
    store_->put_outbox(send);
    store_->mark_outbox_complete(MessageId{42});
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().size(), 0u);
}

TEST_F(InMemoryDeliveryStoreTest, PutAndCheckInbox) {
    auto result = store_->put_inbox(MessageId{100}, 60'000'000'000ULL);
    EXPECT_TRUE(result.ok());
    auto seen = store_->seen_inbox(MessageId{100});
    ASSERT_TRUE(seen.ok());
    EXPECT_TRUE(seen.value());
    auto not_seen = store_->seen_inbox(MessageId{999});
    ASSERT_TRUE(not_seen.ok());
    EXPECT_FALSE(not_seen.value());
}

TEST_F(InMemoryDeliveryStoreTest, EmptyOutboxOnStartup) {
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().size(), 0u);
}

// ===========================================================================
// FileDeliveryStore tests
// ===========================================================================

class FileDeliveryStoreTest : public ::testing::Test {
  protected:
    void SetUp() override {
        char tmpl[] = "/tmp/hpactor_deliv_test_XXXXXX";
        auto* dir = mkdtemp(tmpl);
        ASSERT_NE(dir, nullptr);
        test_dir_ = dir;
        store_ = std::make_unique<FileDeliveryStore>(test_dir_);
    }
    void TearDown() override {
        store_.reset();
        std::filesystem::remove_all(test_dir_);
    }
    std::unique_ptr<FileDeliveryStore> store_;
    std::string test_dir_;
};

TEST_F(FileDeliveryStoreTest, PutAndLoadOutbox) {
    auto send = msg::PendingSend{MessageId{42}};
    auto result = store_->put_outbox(send);
    EXPECT_TRUE(result.ok());
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().size(), 1u);
}

TEST_F(FileDeliveryStoreTest, MarkCompleteRemoves) {
    auto send = msg::PendingSend{MessageId{42}};
    store_->put_outbox(send);
    store_->mark_outbox_complete(MessageId{42});
    auto loaded = store_->load_pending_outbox();
    ASSERT_TRUE(loaded.ok());
    EXPECT_EQ(loaded.value().size(), 0u);
}

TEST_F(FileDeliveryStoreTest, PutAndCheckInbox) {
    store_->put_inbox(MessageId{100}, 60'000'000'000ULL);
    auto seen = store_->seen_inbox(MessageId{100});
    ASSERT_TRUE(seen.ok());
    EXPECT_TRUE(seen.value());
}

} // namespace hpactor::mailbox
