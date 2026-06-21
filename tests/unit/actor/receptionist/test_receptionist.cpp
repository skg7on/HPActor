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

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/receptionist/receptionist.hpp>
#include <hpactor/actor/receptionist/receptionist_messages.hpp>
#include <hpactor/actor/receptionist/service_key.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "system_test_fixture.hpp"

using namespace hpactor;
using namespace hpactor::receptionist;

// ── ServiceKey ───────────────────────────────────────────────────

TEST(ServiceKeyTest, EqualityByName) {
    ServiceKey a{"worker"};
    ServiceKey b{"worker"};
    ServiceKey c{"other"};
    EXPECT_EQ(a, b);
    EXPECT_NE(a, c);
}

TEST(ServiceKeyTest, HashByName) {
    std::hash<ServiceKey> h;
    ServiceKey a{"worker"};
    ServiceKey b{"worker"};
    EXPECT_EQ(h(a), h(b));
}

TEST(ServiceKeyTest, DefaultTypeTagIsZero) {
    ServiceKey k{"test"};
    EXPECT_EQ(k.type_tag, 0u);
}

// ── Listing ──────────────────────────────────────────────────────

TEST(ListingTest, EmptyListingHasNoAddresses) {
    Listing listing;
    listing.key = ServiceKey{"test"};
    EXPECT_TRUE(listing.addresses.empty());
    EXPECT_EQ(listing.key.name, "test");
}

// ── Receptionist Actor ───────────────────────────────────────────

class ReceptionistTest : public ::testing::Test {
  protected:
    void SetUp() override {
        auto cfg = test::config_with_scheduler(1);
        cfg.enable_receptionist = true;
        system_ = std::make_unique<ActorSystem>(cfg);
        rec_ = system_->receptionist();
        ASSERT_NE(rec_, nullptr);
    }

    std::unique_ptr<ActorSystem> system_;
    Receptionist* rec_;
};

TEST_F(ReceptionistTest, StartsWithZeroRegistrations) {
    EXPECT_EQ(rec_->registration_count(), 0u);
}

TEST_F(ReceptionistTest, StartsWithZeroSubscribers) {
    EXPECT_EQ(rec_->subscriber_count(), 0u);
}

TEST_F(ReceptionistTest, RegisterAddsToRegistry) {
    ServiceKey key{"test-service"};
    // Use the system's own endpoint and a synthetic actor id
    ActorAddress addr{system_->endpoint(), 1u, ActorId{1}, 0};
    rec_->register_actor(key, addr);
    EXPECT_EQ(rec_->registration_count(), 1u);
}

TEST_F(ReceptionistTest, RegisterDuplicateIsIdempotent) {
    ServiceKey key{"test-service"};
    ActorAddress addr{system_->endpoint(), 1u, ActorId{1}, 0};
    rec_->register_actor(key, addr);
    rec_->register_actor(key, addr);
    EXPECT_EQ(rec_->registration_count(), 1u);
}

TEST_F(ReceptionistTest, UnregisterRemovesFromRegistry) {
    ServiceKey key{"test-service"};
    ActorAddress addr{system_->endpoint(), 1u, ActorId{1}, 0};
    rec_->register_actor(key, addr);
    EXPECT_EQ(rec_->registration_count(), 1u);
    rec_->unregister_actor(key, addr);
    EXPECT_EQ(rec_->registration_count(), 0u);
}

TEST_F(ReceptionistTest, RegisterDifferentKeys) {
    ServiceKey key_a{"a"};
    ServiceKey key_b{"b"};
    ActorAddress addr{system_->endpoint(), 1u, ActorId{1}, 0};
    rec_->register_actor(key_a, addr);
    rec_->register_actor(key_b, addr);
    EXPECT_EQ(rec_->registration_count(), 2u);
}

TEST_F(ReceptionistTest, GetListingReturnsRegisteredActors) {
    ServiceKey key{"test-service"};
    ActorAddress addr1{system_->endpoint(), 1u, ActorId{1}, 0};
    ActorAddress addr2{system_->endpoint(), 1u, ActorId{2}, 0};
    rec_->register_actor(key, addr1);
    rec_->register_actor(key, addr2);

    auto listing = rec_->get_listing(key);
    EXPECT_EQ(listing.addresses.size(), 2u);
}

TEST_F(ReceptionistTest, GetListingForUnknownKeyReturnsEmpty) {
    ServiceKey key{"nonexistent"};
    auto listing = rec_->get_listing(key);
    EXPECT_TRUE(listing.addresses.empty());
}

TEST_F(ReceptionistTest, SubscribeAddsSubscriber) {
    ServiceKey key{"test-service"};
    ActorAddress sub{system_->endpoint(), 1u, ActorId{100}, 0};
    rec_->add_subscriber(key, sub);
    EXPECT_EQ(rec_->subscriber_count(), 1u);
}

TEST_F(ReceptionistTest, UnsubscribeRemovesSubscriber) {
    ServiceKey key{"test-service"};
    ActorAddress sub{system_->endpoint(), 1u, ActorId{100}, 0};
    rec_->add_subscriber(key, sub);
    EXPECT_EQ(rec_->subscriber_count(), 1u);
    rec_->remove_subscriber(key, sub);
    EXPECT_EQ(rec_->subscriber_count(), 0u);
}

TEST_F(ReceptionistTest, UnregisterNonexistentIsSafe) {
    ServiceKey key{"test-service"};
    ActorAddress addr{system_->endpoint(), 1u, ActorId{1}, 0};
    rec_->unregister_actor(key, addr);
    EXPECT_EQ(rec_->registration_count(), 0u);
}

TEST_F(ReceptionistTest, RegisterThenUnregisterThenGetListingIsEmpty) {
    ServiceKey key{"test-service"};
    ActorAddress addr{system_->endpoint(), 1u, ActorId{1}, 0};
    rec_->register_actor(key, addr);
    rec_->unregister_actor(key, addr);
    auto listing = rec_->get_listing(key);
    EXPECT_TRUE(listing.addresses.empty());
}

TEST_F(ReceptionistTest, MultipleSubscribersForSameKey) {
    ServiceKey key{"test-service"};
    ActorAddress sub1{system_->endpoint(), 1u, ActorId{100}, 0};
    ActorAddress sub2{system_->endpoint(), 1u, ActorId{200}, 0};
    ActorAddress sub3{system_->endpoint(), 1u, ActorId{300}, 0};
    rec_->add_subscriber(key, sub1);
    rec_->add_subscriber(key, sub2);
    rec_->add_subscriber(key, sub3);
    EXPECT_EQ(rec_->subscriber_count(), 3u);
}
