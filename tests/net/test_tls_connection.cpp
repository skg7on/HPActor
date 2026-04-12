#include <hpactor/net/tls_connection.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test TlsMessageType enum
    assert(static_cast<uint8_t>(TlsMessageType::ClientHello) == 1);
    assert(static_cast<uint8_t>(TlsMessageType::ServerHello) == 2);
    assert(static_cast<uint8_t>(TlsMessageType::Certificate) == 3);
    assert(static_cast<uint8_t>(TlsMessageType::CertificateVerify) == 4);
    assert(static_cast<uint8_t>(TlsMessageType::Finished) == 5);

    // Test TlsHandshakeState enum
    assert(static_cast<uint8_t>(TlsHandshakeState::Idle) == 0);
    assert(static_cast<uint8_t>(TlsHandshakeState::WaitingForServerHello) == 1);
    assert(static_cast<uint8_t>(TlsHandshakeState::WaitingForCertificate) == 2);
    assert(static_cast<uint8_t>(TlsHandshakeState::WaitingForCertificateVerify) == 3);
    assert(static_cast<uint8_t>(TlsHandshakeState::WaitingForFinished) == 4);
    assert(static_cast<uint8_t>(TlsHandshakeState::HandshakeComplete) == 5);
    assert(static_cast<uint8_t>(TlsHandshakeState::Error) == 6);

    // Test TlsSessionState enum
    assert(static_cast<uint8_t>(TlsSessionState::Handshake) == 0);
    assert(static_cast<uint8_t>(TlsSessionState::Encrypted) == 1);
    assert(static_cast<uint8_t>(TlsSessionState::Error) == 2);

    // Test Nonce size
    Nonce nonce{};
    assert(nonce.size() == kNonceSize);

    return 0;
}
