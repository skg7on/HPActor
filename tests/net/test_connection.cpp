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
