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

#include <hpactor/net/transport.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <cassert>

using namespace hpactor;

int main() {
    // Test ActorProxy construction with invalid address
    ActorProxy proxy1(ActorAddress{}, static_cast<net::Transport*>(nullptr));
    assert(!proxy1);
    assert(proxy1.address().endpoint == endpoint_ops::parse_endpoint(""));
    assert(proxy1.endpoint() == endpoint_ops::parse_endpoint(""));
    assert(!proxy1.is_local());

    // Test ActorProxy with valid address
    ActorId id(42);
    ActorAddress addr(endpoint_ops::parse_endpoint("remotehost:12345"), 0, id,
                      0); // remote node
    ActorProxy proxy2(addr, static_cast<net::Transport*>(nullptr));
    assert(proxy2);
    assert(proxy2.address() == addr);
    assert(proxy2.endpoint() == endpoint_ops::parse_endpoint("remotehost:"
                                                             "12345"));
    assert(!proxy2.is_local());

    return 0;
}
