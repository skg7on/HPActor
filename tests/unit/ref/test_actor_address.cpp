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

#include <cstdint>
#include <functional>
#include <gtest/gtest.h>
#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

TEST(ActorAddressTest, Default) {
    hpactor::ActorAddress addr;
    EXPECT_FALSE(addr);
    EXPECT_EQ(addr.id.value(), 0);
}

TEST(ActorAddressTest, Local) {
    hpactor::ActorId id(1);
    hpactor::ActorAddress addr{hpactor::LocalEndpoint, 0, id, 0};
    EXPECT_TRUE(addr.is_local());
}

TEST(ActorAddressTest, Equality) {
    hpactor::ActorId id1(1), id2(1), id3(2);
    hpactor::ActorAddress a{hpactor::LocalEndpoint, 0, id1, 0};
    hpactor::ActorAddress b{hpactor::LocalEndpoint, 0, id2, 0};
    hpactor::ActorAddress c{hpactor::LocalEndpoint, 0, id3, 0};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a == c);
}

TEST(ActorAddressTest, Inequality) {
    hpactor::ActorId id1(1), id2(2);
    hpactor::ActorAddress a{hpactor::LocalEndpoint, 0, id1, 0};
    hpactor::ActorAddress b{hpactor::LocalEndpoint, 0, id2, 0};
    EXPECT_TRUE(a != b);
}

TEST(ActorAddressTest, Remote) {
    hpactor::ActorId id(1);
    hpactor::ActorAddress addr{hpactor::endpoint_ops::parse_endpoint("remotehos"
                                                                     "t:12345"),
                               0, id, 0};
    EXPECT_FALSE(addr.is_local());
}

TEST(ActorAddressTest, Incarnation) {
    hpactor::ActorId id(1);
    hpactor::ActorAddress a{hpactor::LocalEndpoint, 0, id, 0};
    hpactor::ActorAddress b{hpactor::LocalEndpoint, 0, id, 1};
    EXPECT_TRUE(a != b); // Same id but different incarnation
}

TEST(ActorAddressTest, Invalid) {
    hpactor::ActorAddr invalid = hpactor::invalid_actor_addr;
    EXPECT_FALSE(invalid);
    EXPECT_EQ(invalid.id.value(), 0);
}

TEST(ActorAddressTest, Hash) {
    hpactor::ActorId id1(1), id2(2);
    hpactor::ActorAddress a{hpactor::LocalEndpoint, 0, id1, 0};
    hpactor::ActorAddress b{hpactor::LocalEndpoint, 0, id1, 0};
    hpactor::ActorAddress c{hpactor::LocalEndpoint, 0, id2, 0};

    std::hash<hpactor::ActorAddress> hasher;
    EXPECT_EQ(hasher(a), hasher(b));
    EXPECT_NE(hasher(a), hasher(c));
}
