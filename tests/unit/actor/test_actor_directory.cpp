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

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/actor/actor_system.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/event_based_actor.hpp>

#include <gtest/gtest.h>

using namespace hpactor;

namespace {

class DirectoryTestActor final : public EventBasedActor {
  public:
    DirectoryTestActor(ActorContext* ctx, ActorSystem& sys)
        : EventBasedActor(ctx, sys) {}

    Behavior make_behavior() override {
        return {};
    }
};

} // namespace

TEST(ActorDirectoryTest, AllocateIdIncrements) {
    ActorDirectory directory;
    auto first = directory.allocate_id();
    auto second = directory.allocate_id();
    EXPECT_NE(first, second);
    EXPECT_LT(first.value(), second.value());
}

TEST(ActorDirectoryTest, InsertAndFindEntry) {
    ActorDirectory directory;
    Config config;
    ActorSystem system{config};
    auto instance = std::make_shared<DirectoryTestActor>(nullptr, system);
    instance->set_address(
        ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{42}, 0});
    Actor actor{instance};
    auto mailbox = std::make_shared<mailbox::MPSCActorMailbox<TypedMessage>>(
        ActorId{42}, nullptr, mailbox::MailboxConfig{});
    auto context = std::shared_ptr<ActorContext>{};

    ASSERT_TRUE(directory.insert({actor, instance, mailbox, context}));

    auto found = directory.find(ActorId{42});
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(found->actor.id(), ActorId{42});
    EXPECT_EQ(found->instance, instance);
    EXPECT_EQ(found->mailbox, mailbox);
}

TEST(ActorDirectoryTest, ResolveActorByName) {
    ActorDirectory directory;
    Config config;
    ActorSystem system{config};
    auto instance = std::make_shared<DirectoryTestActor>(nullptr, system);
    instance->set_address(
        ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{7}, 0});
    Actor actor{instance};
    ASSERT_TRUE(directory.insert({actor, nullptr, nullptr, nullptr}));
    ASSERT_TRUE(directory.register_name("service", actor.address()));

    auto resolved = directory.resolve_actor("service");
    ASSERT_TRUE(resolved.has_value());
    EXPECT_EQ(resolved->id(), ActorId{7});
}

TEST(ActorDirectoryTest, UnregisterNameRemovesMapping) {
    ActorDirectory directory;
    ActorAddress address{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{7}, 0};
    ASSERT_TRUE(directory.register_name("service", address));

    EXPECT_TRUE(directory.unregister_name("service"));
    EXPECT_FALSE(directory.resolve_name("service").has_value());
    EXPECT_FALSE(directory.unregister_name("service"));
}

TEST(ActorDirectoryTest, DuplicateNameKeepsFirstAddress) {
    ActorDirectory directory;
    ActorAddress first{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{7}, 0};
    ActorAddress second{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{8}, 0};

    EXPECT_TRUE(directory.register_name("service", first));
    EXPECT_FALSE(directory.register_name("service", second));
    ASSERT_TRUE(directory.resolve_name("service").has_value());
    EXPECT_EQ(directory.resolve_name("service")->id, first.id);
}

TEST(ActorDirectoryTest, EraseActorRemovesAllNamesForActor) {
    ActorDirectory directory;
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system{config};
    auto instance = std::make_shared<DirectoryTestActor>(nullptr, system);
    instance->set_address(
        ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{9}, 0});
    Actor actor{instance};
    ASSERT_TRUE(directory.insert({actor, instance, nullptr, nullptr}));
    ASSERT_TRUE(directory.register_name("primary", actor.address()));
    ASSERT_TRUE(directory.register_name("alias", actor.address()));

    EXPECT_TRUE(directory.erase(actor.id()));
    EXPECT_FALSE(directory.resolve_name("primary").has_value());
    EXPECT_FALSE(directory.resolve_name("alias").has_value());
}

TEST(ActorDirectoryTest, PublishCommitsEntryAndNameTogether) {
    ActorDirectory directory;
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system{config};
    auto instance = std::make_shared<DirectoryTestActor>(nullptr, system);
    instance->set_address(
        ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, ActorId{41}, 0});
    Actor actor{instance};
    ActorDirectoryEntry entry{actor, instance, nullptr, nullptr};

    auto status = directory.publish(std::move(entry), "worker-41");

    EXPECT_EQ(status, ActorDirectory::PublishStatus::Published);
    ASSERT_TRUE(directory.find(ActorId{41}).has_value());
    ASSERT_TRUE(directory.resolve_name("worker-41").has_value());
    EXPECT_EQ(directory.resolve_name("worker-41")->id, ActorId{41});
}

TEST(ActorDirectoryTest, DuplicateNamePublishesNoOrphanEntry) {
    ActorDirectory directory;
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system{config};

    auto make_entry = [&](ActorId id) {
        auto inst = std::make_shared<DirectoryTestActor>(nullptr, system);
        inst->set_address(
            ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, id, 0});
        return ActorDirectoryEntry{Actor{inst}, inst, nullptr, nullptr};
    };

    ASSERT_EQ(directory.publish(make_entry(ActorId{41}), "worker"),
              ActorDirectory::PublishStatus::Published);

    EXPECT_EQ(directory.publish(make_entry(ActorId{42}), "worker"),
              ActorDirectory::PublishStatus::DuplicateName);
    EXPECT_FALSE(directory.find(ActorId{42}).has_value());
    EXPECT_EQ(directory.size(), 1U);
}

TEST(ActorDirectoryTest, DuplicateIdPublishesNoSecondName) {
    ActorDirectory directory;
    Config config;
    config.scheduler_threads = 0;
    ActorSystem system{config};

    auto make_entry = [&](ActorId id) {
        auto inst = std::make_shared<DirectoryTestActor>(nullptr, system);
        inst->set_address(
            ActorAddress{EndPoint{LocalEndpoint}, ActorType{1}, id, 0});
        return ActorDirectoryEntry{Actor{inst}, inst, nullptr, nullptr};
    };

    ASSERT_EQ(directory.publish(make_entry(ActorId{41}), "first"),
              ActorDirectory::PublishStatus::Published);

    EXPECT_EQ(directory.publish(make_entry(ActorId{41}), "second"),
              ActorDirectory::PublishStatus::DuplicateActorId);
    EXPECT_FALSE(directory.resolve_name("second").has_value());
}
