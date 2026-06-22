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
#include <hpactor/cluster/singleton/singleton_identity.hpp>

namespace hpactor::cluster::singleton {

TEST(SingletonIdentityTest, DefaultConstruction) {
    SingletonIdentity id;
    EXPECT_TRUE(id.name.empty());
    EXPECT_EQ(id.fencing_token, 0);
}

TEST(SingletonIdentityTest, NamedConstruction) {
    SingletonIdentity id{"shard-coordinator", 5};
    EXPECT_EQ(id.name, "shard-coordinator");
    EXPECT_EQ(id.fencing_token, 5);
}

TEST(SingletonIdentityTest, IdentityComparison) {
    SingletonIdentity a{"singleton-a", 1};
    SingletonIdentity b{"singleton-a", 1};
    SingletonIdentity c{"singleton-b", 1};
    EXPECT_EQ(a.name, b.name);
    EXPECT_NE(a.name, c.name);
}

TEST(SingletonStateTest, AllStatesDefined) {
    EXPECT_NE(static_cast<uint8_t>(SingletonState::Standby),
              static_cast<uint8_t>(SingletonState::Active));
    EXPECT_NE(static_cast<uint8_t>(SingletonState::Activating),
              static_cast<uint8_t>(SingletonState::Draining));
}

TEST(SingletonStateTest, ToStringReturnsExpected) {
    EXPECT_STREQ(to_string(SingletonState::Standby), "standby");
    EXPECT_STREQ(to_string(SingletonState::Active), "active");
    EXPECT_STREQ(to_string(SingletonState::Activating), "activating");
    EXPECT_STREQ(to_string(SingletonState::Draining), "draining");
}

} // namespace hpactor::cluster::singleton
