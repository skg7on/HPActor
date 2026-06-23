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
#include <hpactor/cluster/receptionist/cluster_receptionist_actor.hpp>

namespace hpactor::cluster::receptionist {

using hpactor::receptionist::ServiceKey;

TEST(ClusterReceptionistActorTest, DefaultConstruction) {
    ClusterReceptionistActor actor;
    EXPECT_EQ(actor.local_key_count(), 0);
    EXPECT_EQ(actor.remote_key_count(), 0);
}

TEST(ClusterReceptionistActorTest, RegisterAndQueryLocal) {
    ClusterReceptionistActor actor;
    ServiceKey key{"storage"};
    actor.apply_local_registration(key, {10, 20, 30});
    auto listing = actor.get_cluster_listing(key);
    EXPECT_EQ(listing.size(), 3);
}

TEST(ClusterReceptionistActorTest, MergeRemoteAndList) {
    ClusterReceptionistActor actor;
    ServiceKey key{"proxy"};
    actor.apply_local_registration(key, {1});
    actor.merge_remote_registration(ClusterRegistration{key, "node-r", {500}, 1});
    auto listing = actor.get_cluster_listing(key);
    EXPECT_EQ(listing.size(), 2);
}

TEST(ClusterReceptionistActorTest, RemoveNodeCleansUp) {
    ClusterReceptionistActor actor;
    ServiceKey key{"cleanup"};
    actor.merge_remote_registration(ClusterRegistration{key, "dead-node", {99}, 0});
    actor.remove_node_registrations("dead-node");
    EXPECT_FALSE(actor.has_key(key));
}

TEST(ClusterReceptionistActorTest, AccessCore) {
    ClusterReceptionistActor actor;
    ServiceKey key{"via-core"};
    actor.core().apply_local_registration(key, {7});
    EXPECT_EQ(actor.core().local_key_count(), 1);
}

} // namespace hpactor::cluster::receptionist
