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

#include <cassert>
#include <cstdint>
#include <functional>
#include <hpactor/actor/actor_fwd.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/types/types.hpp>

void test_actor_address_default() {
    hpactor::ActorAddress addr;
    assert(!addr);
    assert(addr.id.value() == 0);
}

void test_actor_address_local() {
    hpactor::ActorId id(1);
    hpactor::ActorAddress addr{hpactor::LocalNodeId, 0, id, 0};
    assert(addr.is_local());
}

void test_actor_address_equality() {
    hpactor::ActorId id1(1), id2(1), id3(2);
    hpactor::ActorAddress a{hpactor::LocalNodeId, 0, id1, 0};
    hpactor::ActorAddress b{hpactor::LocalNodeId, 0, id2, 0};
    hpactor::ActorAddress c{hpactor::LocalNodeId, 0, id3, 0};
    assert(a == b);
    assert(!(a == c));
}

void test_actor_address_inequality() {
    hpactor::ActorId id1(1), id2(2);
    hpactor::ActorAddress a{hpactor::LocalNodeId, 0, id1, 0};
    hpactor::ActorAddress b{hpactor::LocalNodeId, 0, id2, 0};
    assert(a != b);
}

void test_actor_address_remote() {
    hpactor::ActorId id(1);
    hpactor::ActorAddress addr{"remotehost:12345", 0, id, 0};
    assert(!addr.is_local());
}

void test_actor_address_incarnation() {
    hpactor::ActorId id(1);
    hpactor::ActorAddress a{hpactor::LocalNodeId, 0, id, 0};
    hpactor::ActorAddress b{hpactor::LocalNodeId, 0, id, 1};
    assert(a != b); // Same id but different incarnation
}

void test_actor_address_invalid() {
    hpactor::ActorAddr invalid = hpactor::invalid_actor_addr;
    assert(!invalid);
    assert(invalid.id.value() == 0);
}

void test_actor_address_hash() {
    hpactor::ActorId id1(1), id2(2);
    hpactor::ActorAddress a{hpactor::LocalNodeId, 0, id1, 0};
    hpactor::ActorAddress b{hpactor::LocalNodeId, 0, id1, 0};
    hpactor::ActorAddress c{hpactor::LocalNodeId, 0, id2, 0};

    std::hash<hpactor::ActorAddress> hasher;
    assert(hasher(a) == hasher(b));
    assert(hasher(a) != hasher(c));
}

int main() {
    test_actor_address_default();
    test_actor_address_local();
    test_actor_address_equality();
    test_actor_address_inequality();
    test_actor_address_remote();
    test_actor_address_incarnation();
    test_actor_address_invalid();
    test_actor_address_hash();

    return 0;
}