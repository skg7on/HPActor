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

#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/common.pb.h>

#include <cassert>
#include <cstdio>

using namespace hpactor;

int main() {
    printf("=== ProtoTypeRegistry Tests ===\n");

    // Test 1: Construction — no types registered by default
    {
        printf("Test 1: construction... ");
        ProtoTypeRegistry reg;
        assert(!reg.has_tag(TypeTag::User));
        printf("PASS\n");
    }

    // Test 2: Register a type and verify tag + name lookup
    {
        printf("Test 2: register_type + has_tag + type_name... ");
        ProtoTypeRegistry reg;
        reg.register_type<PbActorRef>(TypeTag::User, "hpactor.PbActorRef");
        assert(reg.has_tag(TypeTag::User));
        assert(reg.type_name(TypeTag::User) == "hpactor.PbActorRef");
        printf("PASS\n");
    }

    // Test 3: create() returns non-null for registered type with matching name
    {
        printf("Test 3: create registered type... ");
        ProtoTypeRegistry reg;
        reg.register_type<PbActorRef>(TypeTag::User, "hpactor.PbActorRef");
        auto msg = reg.create(TypeTag::User);
        assert(msg != nullptr);
        assert(msg->GetTypeName() == "hpactor.PbActorRef");
        printf("PASS\n");
    }

    // Test 4: create() returns nullptr for unregistered tag
    {
        printf("Test 4: create unregistered tag... ");
        ProtoTypeRegistry reg;
        auto msg = reg.create(static_cast<TypeTag>(999));
        assert(msg == nullptr);
        printf("PASS\n");
    }

    // Test 5: deserialize() fails on tag not registered
    {
        printf("Test 5: deserialize unregistered tag... ");
        ProtoTypeRegistry reg;
        bytes data = {0x00, 0x00, 0x00, 0x01, 0x00};
        auto msg = reg.deserialize(static_cast<TypeTag>(1), data);
        assert(msg == nullptr);
        printf("PASS\n");
    }

    // Test 6: Wire encode then decode round-trip with actual protobuf message
    {
        printf("Test 6: wire encode/decode round-trip... ");
        ProtoTypeRegistry reg;
        reg.register_type<PbActorRef>(TypeTag::User, "hpactor.PbActorRef");

        PbActorRef ref;
        ref.set_type(1);
        ref.set_actor_id(42);
        ref.set_incarnation(0);

        bytes wire = reg.encode_wire(TypeTag::User, ref);
        assert(wire.size() > 4);

        auto [tag, msg] = reg.decode_wire(wire);
        assert(tag == TypeTag::User);
        assert(msg != nullptr);
        auto* decoded = static_cast<PbActorRef*>(msg.get());
        assert(decoded != nullptr);
        assert(decoded->type() == 1);
        assert(decoded->actor_id() == 42);
        assert(decoded->incarnation() == 0);
        printf("PASS\n");
    }

    // Test 7: Decode short buffer returns invalid
    {
        printf("Test 7: short buffer decode... ");
        ProtoTypeRegistry reg;
        bytes short_buf = {0x00, 0x00};
        auto [tag, msg] = reg.decode_wire(short_buf);
        assert(tag == TypeTag::Invalid);
        assert(msg == nullptr);
        printf("PASS\n");
    }

    // Test 8: lookup<T>() finds registered type
    {
        printf("Test 8: lookup existing type... ");
        ProtoTypeRegistry reg;
        reg.register_type<PbActorRef>(TypeTag::User, "hpactor.PbActorRef");
        TypeTag found = reg.lookup<PbActorRef>();
        assert(found == TypeTag::User);
        printf("PASS\n");
    }

    // Test 9: lookup<T>() returns Invalid for unregistered type
    {
        printf("Test 9: lookup unregistered type... ");
        // Use a different protobuf type that hasn't been registered
        // (register only PbActorRef, lookup a different type)
        ProtoTypeRegistry reg;
        // Don't register anything — verify no false positives
        auto it = reg.lookup<PbActorRef>();
        assert(it == TypeTag::Invalid);
        printf("PASS\n");
    }

    printf("=== All ProtoTypeRegistry Tests Passed ===\n");
    return 0;
}
