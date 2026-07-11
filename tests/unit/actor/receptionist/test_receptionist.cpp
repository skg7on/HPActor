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

#include <hpactor/actor/receptionist/receptionist.hpp>
#include <hpactor/actor/receptionist/receptionist_messages.hpp>
#include <hpactor/actor/receptionist/service_key.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/typed_message.hpp>

#include <gtest/gtest.h>

#include <memory>

#include "scheduler_test_driver.hpp"
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

// ── ActorContext Convenience Method Tests ─────────────────────────

namespace {

/// Test actor that exercises receptionist convenience methods via
/// ActorContext.  Responds to TypeTag values to trigger different
/// operations:
///   - tag 100: receptionist_register
///   - tag 101: receptionist_subscribe
///   - tag 102: receptionist_unregister
///   - tag 103: receptionist_unsubscribe
class ReceptionistMethodActor : public EventBasedActor {
  public:
    ServiceKey key;
    bool registered{false};
    bool unregistered{false};
    bool subscribed{false};
    bool unsubscribed{false};

    ReceptionistMethodActor(ActorContext* ctx, ActorSystem& sys, ServiceKey k)
        : EventBasedActor(ctx, sys), key(std::move(k)) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior{[this](TypedMessage& msg) {
            auto tag = static_cast<uint32_t>(msg.type_id());
            switch (tag) {
                case 100:
                    context()->receptionist_register(key);
                    registered = true;
                    break;
                case 101:
                    context()->receptionist_subscribe(key);
                    subscribed = true;
                    break;
                case 102:
                    context()->receptionist_unregister(key);
                    unregistered = true;
                    break;
                case 103:
                    context()->receptionist_unsubscribe(key);
                    unsubscribed = true;
                    break;
                default:
                    break;
            }
        }};
    }
};

} // namespace

/// Fixture that adds a SchedulerTestDriver so spawned actors can
/// process messages deterministically.
class ReceptionistContextTest : public ReceptionistTest {
  protected:
    void SetUp() override {
        ReceptionistTest::SetUp();
        driver_ = std::make_unique<test::SchedulerTestDriver>(*system_);
    }

    void TearDown() override {
        driver_.reset();
        ReceptionistTest::TearDown();
    }

    /// Send a message with \p tag to \p target and drain the scheduler.
    void send_and_drain(const ActorAddress& target, uint32_t tag) {
        TypedMessage msg(TypeTag(tag), StreamBuffer{});
        system_->try_deliver_local(target.id, std::move(msg));
        driver_->drain(100);
    }

    std::unique_ptr<test::SchedulerTestDriver> driver_;
};

TEST_F(ReceptionistContextTest, RegisterViaActorContext) {
    ServiceKey key{"ctx-test"};
    auto actor = system_->spawn<ReceptionistMethodActor>(key);
    driver_->drain(100);

    send_and_drain(actor.address(), 100);

    auto listing = rec_->get_listing(key);
    ASSERT_EQ(listing.addresses.size(), 1u);
    EXPECT_EQ(listing.addresses[0], actor.address());
}

TEST_F(ReceptionistContextTest, SubscribeViaActorContext) {
    ServiceKey key{"ctx-sub"};
    auto actor = system_->spawn<ReceptionistMethodActor>(key);
    driver_->drain(100);

    send_and_drain(actor.address(), 101);

    EXPECT_EQ(rec_->subscriber_count(), 1u);
}

TEST_F(ReceptionistContextTest, UnregisterViaActorContext) {
    ServiceKey key{"ctx-unreg"};
    auto actor = system_->spawn<ReceptionistMethodActor>(key);
    driver_->drain(100);

    // First register.
    send_and_drain(actor.address(), 100);
    ASSERT_EQ(rec_->registration_count(), 1u);

    // Then unregister.
    send_and_drain(actor.address(), 102);

    EXPECT_EQ(rec_->registration_count(), 0u);
    auto listing = rec_->get_listing(key);
    EXPECT_TRUE(listing.addresses.empty());
}

TEST_F(ReceptionistContextTest, UnsubscribeViaActorContext) {
    ServiceKey key{"ctx-unsub"};
    auto actor = system_->spawn<ReceptionistMethodActor>(key);
    driver_->drain(100);

    // First subscribe.
    send_and_drain(actor.address(), 101);
    ASSERT_EQ(rec_->subscriber_count(), 1u);

    // Then unsubscribe.
    send_and_drain(actor.address(), 103);

    EXPECT_EQ(rec_->subscriber_count(), 0u);
}
