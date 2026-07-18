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

#include <hpactor/actor/system/actor_directory.hpp>
#include <hpactor/cluster/name/name_registration_port.hpp>

using namespace hpactor;

namespace {

TEST(ActorDirectoryPortTest, RegistrationPortCalledOnPublish) {
    ActorDirectory dir;
    bool register_called = false;

    cluster::name::NameRegistrationPort port;
    port.context = &register_called;
    port.on_register = [](void* ctx, std::string_view /*name*/,
                           ActorAddress /*addr*/, uint64_t /*gen*/) {
        auto* flag = static_cast<bool*>(ctx);
        *flag = true;
    };
    dir.set_name_registration_port(port);

    // Publish with a name.
    (void)dir.allocate_id();
    ActorDirectoryEntry entry;
    entry.actor = Actor{};
    dir.publish(std::move(entry), "test-actor");

    EXPECT_TRUE(register_called);
}

TEST(ActorDirectoryPortTest, UnregisterPortCalledOnErase) {
    ActorDirectory dir;
    bool unregister_called = false;

    cluster::name::NameRegistrationPort port;
    port.context = &unregister_called;
    port.on_unregister = [](void* ctx, std::string_view /*name*/) {
        *static_cast<bool*>(ctx) = true;
    };
    dir.set_name_registration_port(port);

    (void)dir.allocate_id();
    ActorDirectoryEntry entry;
    entry.actor = Actor{};
    auto entry_id = entry.actor.id();
    dir.publish(std::move(entry), "to-be-erased");
    dir.erase(entry_id);

    EXPECT_TRUE(unregister_called);
}

TEST(ActorDirectoryPortTest, PortNotSetIsNoOp) {
    ActorDirectory dir;
    (void)dir.allocate_id();
    ActorDirectoryEntry entry;
    entry.actor = Actor{};
    // No port set — should not crash.
    EXPECT_EQ(dir.publish(std::move(entry), "no-port-name"),
              ActorDirectory::PublishStatus::Published);
}

} // namespace
