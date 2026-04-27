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
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/common.pb.h> // for test protobuf types

int main() {
    // Test TypedMessage from protobuf message
    ::hpactor::PbIpv4Endpoint ep;
    ep.set_addr(0x7F000001);
    ep.set_port(8080);

    hpactor::TypedMessage msg(hpactor::TypeTag::User, ep);
    assert(msg.type_id() == hpactor::TypeTag::User);
    assert(!msg.payload().empty());

    // Test lazy deserialization with as<T>()
    auto parsed = msg.as<::hpactor::PbIpv4Endpoint>();
    assert(parsed != nullptr);
    assert(parsed->addr() == 0x7F000001);
    assert(parsed->port() == 8080);

    // Test parsed() access after as<T>() caches result
    assert(msg.parsed() != nullptr);

    // Test move preserves parsed state
    auto msg2 = std::move(msg);
    assert(msg2.parsed() != nullptr);
    assert(msg2.payload().size() > 0);

    return 0;
}
