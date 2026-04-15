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

#include <cassert>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test ConnectionState enum
    assert(static_cast<int>(ConnectionState::Disconnected) == 0);
    assert(static_cast<int>(ConnectionState::Connecting) == 1);
    assert(static_cast<int>(ConnectionState::Handshake) == 2);
    assert(static_cast<int>(ConnectionState::Connected) == 3);
    assert(static_cast<int>(ConnectionState::Error) == 4);

    // Test TransportError enum
    assert(static_cast<int>(TransportError::Success) == 0);
    assert(static_cast<int>(TransportError::ConnectionFailed) == 1);
    assert(static_cast<int>(TransportError::Timeout) == 2);
    assert(static_cast<int>(TransportError::SerializationFailed) == 3);
    assert(static_cast<int>(TransportError::BufferOverflow) == 4);
    assert(static_cast<int>(TransportError::NotConnected) == 5);

    return 0;
}
