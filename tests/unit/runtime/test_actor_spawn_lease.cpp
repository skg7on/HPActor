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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/system/actor_directory.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/runtime/actor_spawn_lease.hpp>

using namespace hpactor;

namespace {

// Minimal concrete actor for directory unit tests.
// AbstractActor stores a reference to ActorSystem but never dereferences
// it in id()/address()/type() — the only methods called by ActorDirectory
// during publish/find/name operations.
class TestMinimalActor final : public AbstractActor {
  public:
    explicit TestMinimalActor(uint64_t id_val)
        : AbstractActor(ActorId{id_val}, ActorType{1},
                        *reinterpret_cast<ActorSystem*>(this)) {}
    std::string_view type_name() const override { return "TestMinimalActor"; }
    bool is_event_based_actor() const override { return false; }
    void receive(TypedMessage&) override {}
};

} // namespace

// ── ActorSpawnLease move semantics ──────────────────────────────────────────

TEST(ActorSpawnLeaseTest, DefaultConstructedIsEmpty) {
    ActorSpawnLease lease;
    EXPECT_TRUE(lease.empty());
    EXPECT_FALSE(lease.committed());
    auto result = lease.rollback();
    EXPECT_TRUE(result.ok());
}

TEST(ActorSpawnLeaseTest, MoveConstructedSourceIsDetached) {
    ActorSpawnLease source;
    ActorSpawnLease dest(std::move(source));
    EXPECT_TRUE(dest.empty());
    EXPECT_FALSE(dest.committed());
}

TEST(ActorSpawnLeaseTest, CommitOnEmptyIsNoop) {
    ActorSpawnLease lease;
    auto result = lease.commit();
    EXPECT_TRUE(result.ok());
    EXPECT_FALSE(lease.committed());
}

// ── ActorDirectory atomic batch registration ────────────────────────────────

TEST(ActorDirectoryTest, AtomicNamePublishAllOrNone) {
    ActorDirectory dir;

    ActorDirectoryEntry entry_a;
    entry_a.actor = Actor(std::make_shared<TestMinimalActor>(1));
    ASSERT_EQ(dir.publish(std::move(entry_a)), ActorDirectory::PublishStatus::Published);

    ActorDirectoryEntry entry_b;
    entry_b.actor = Actor(std::make_shared<TestMinimalActor>(2));
    ASSERT_EQ(dir.publish(std::move(entry_b)), ActorDirectory::PublishStatus::Published);

    // Create a name conflict.
    auto addr_a = dir.find_actor(ActorId{1});
    ASSERT_TRUE(addr_a.has_value());
    ASSERT_TRUE(dir.register_name("taken", addr_a.value().address()));

    // Batch: one new name, one conflict → all-or-none.
    ActorDirectory::NamedActor batch[] = {
        {"new_name", ActorId{2}},
        {"taken", ActorId{2}},
    };
    EXPECT_FALSE(dir.register_names_atomically(batch));
    EXPECT_FALSE(dir.resolve_name("new_name").has_value());
}

TEST(ActorDirectoryTest, AtomicNamePublishAllSucceed) {
    ActorDirectory dir;

    ActorDirectoryEntry entry_a;
    entry_a.actor = Actor(std::make_shared<TestMinimalActor>(10));
    ASSERT_EQ(dir.publish(std::move(entry_a)), ActorDirectory::PublishStatus::Published);

    ActorDirectoryEntry entry_b;
    entry_b.actor = Actor(std::make_shared<TestMinimalActor>(20));
    ASSERT_EQ(dir.publish(std::move(entry_b)), ActorDirectory::PublishStatus::Published);

    ActorDirectory::NamedActor batch[] = {
        {"alpha", ActorId{10}},
        {"beta", ActorId{20}},
    };
    EXPECT_TRUE(dir.register_names_atomically(batch));
    EXPECT_TRUE(dir.resolve_name("alpha").has_value());
    EXPECT_TRUE(dir.resolve_name("beta").has_value());
}

TEST(ActorDirectoryTest, AtomicNamePublishRejectsMissingActor) {
    ActorDirectory dir;
    ActorDirectory::NamedActor batch[] = {
        {"ghost", ActorId{999}},
    };
    EXPECT_FALSE(dir.register_names_atomically(batch));
    EXPECT_FALSE(dir.resolve_name("ghost").has_value());
}

TEST(ActorDirectoryTest, AtomicNamePublishTwoNamesForSameActor) {
    ActorDirectory dir;
    ActorDirectoryEntry entry_a;
    entry_a.actor = Actor(std::make_shared<TestMinimalActor>(100));
    ASSERT_EQ(dir.publish(std::move(entry_a)), ActorDirectory::PublishStatus::Published);

    ActorDirectory::NamedActor batch[] = {
        {"name_a", ActorId{100}},
        {"name_b", ActorId{100}},
    };
    EXPECT_TRUE(dir.register_names_atomically(batch));
    EXPECT_TRUE(dir.resolve_name("name_a").has_value());
    EXPECT_TRUE(dir.resolve_name("name_b").has_value());
}
