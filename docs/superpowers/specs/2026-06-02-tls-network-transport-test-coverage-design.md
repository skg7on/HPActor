# TLS Network Transport Test Coverage — Design Spec

**Date:** 2026-06-02
**Status:** Design approved

## Problem

TLS subsystem (`TlsContext`, `TlsConnection`) has very low test coverage. The 26
existing tests across 3 files only verify construction, configuration defaults,
callback wiring, and basic state machine transitions. The actual TLS protocol —
handshake, crypto, encrypted data exchange, and integration with `TcpTransport` —
is untested.

Current test files:

| File | Tests | Coverage |
|------|-------|----------|
| `tests/unit/net/test_tls_context.cpp` | 4 | Enum values, TlsConfig defaults, empty `from_config`, garbage cert |
| `tests/unit/net/test_tls_integration.cpp` | 2 | TlsConfig+PoolConfig defaults, empty context |
| `tests/integration/net/test_tls_connection.cpp` | 20 | create_client/server, state transitions, callbacks, error on wrong-state msg |

## Approach

Two-tier coverage expansion:

1. **Integration tests** — protocol correctness and crypto primitives, using
   `socketpair()` for deterministic client↔server communication. No ActorSystem.
2. **System tests** — TLS wired through `TcpTransport` and `ActorSystem` with
   loopback sockets and ephemeral ports. Validates end-to-end encrypted transport.

## Test Certificate Helper

Both tiers share a helper that generates an RSA key + self-signed X509 cert
using the OpenSSL C API:

```cpp
struct TestCerts {
    StreamBuffer cert_der;    // Self-signed X509 in DER
    StreamBuffer key_der;     // RSA private key in DER (PKCS#8)
    StreamBuffer pub_key_der; // Extracted public key in DER
};
TestCerts generate_test_certs(const char* common_name = "test");
```

This unblocks testing `sign_data`, `decrypt_pre_master_secret`,
`verify_certificate`, and `from_filesystem` with real key material.

---

## Part 1: Integration Tests — Protocol & Crypto

**File:** Extend `tests/integration/net/test_tls_connection.cpp`

### Crypto Primitives (5 tests)

| Test | Verifies |
|------|----------|
| `PrfSha256Determinism` | Same inputs → same 48+ byte output |
| `PrfSha256Length` | Output ≥ 48 bytes for master_secret + key expansion |
| `AesEncryptDecryptRoundtrip` | `decrypt_aes(encrypt_aes(plaintext)) == plaintext` |
| `AesEncryptProducesDifferentCiphertext` | Encryption is non-identity |
| `DeriveSessionKeysProducesKeys` | Keys populated after completed handshake |

### Full Handshake (4 tests)

| Test | Verifies |
|------|----------|
| `CompleteHandshakeClientServer` | Both sides reach `Encrypted`, `ready_handler_` fires |
| `HandshakeMessagesAccumulated` | `handshake_messages_` contains all steps for Finished verify |
| `SessionStateAfterHandshake` | Both sides `TlsSessionState::Encrypted` |
| `ServerHelloInsufficientData` | < 32 bytes → Error state |

### Encrypted Data Exchange (4 tests)

| Test | Verifies |
|------|----------|
| `SingleEncryptedFrame` | Send one frame, receiver's `frame_handler_` gets plaintext |
| `MultipleEncryptedFrames` | N sequential frames, all decrypted in order |
| `LargeEncryptedPayload` | Payload > AES_BLOCK_SIZE, correct roundtrip |
| `SendBeforeHandshakeComplete` | `send()` in Handshake state produces no encrypted output |

### Record Framing (3 tests)

| Test | Verifies |
|------|----------|
| `FragmentedRecordHeader` | Record header split across two reads — waits for more data |
| `MultipleRecordsInOneRead` | Two records in one `read()` — both processed |
| `EmptyReadBuffer` | `< 4 bytes in buffer is safe` |

### TlsContext with Real Keys (5 tests)

| Test | Verifies |
|------|----------|
| `SignDataWithRealKey` | Non-empty signature with real RSA key |
| `DecryptPreMasterSecret` | RSA decrypt of encrypted blob succeeds |
| `VerifyValidCertificate` | Self-signed cert → `Ok` |
| `VerifyInvalidCertificate` | Garbage DER → `Invalid` |
| `FromFilesystemLoadsCerts` | Temp PEM files → cert + key loaded |

### TlsConnection Lifecycle (3 tests)

| Test | Verifies |
|------|----------|
| `MoveConstructedConnection` | Move ctor preserves state and fd |
| `MoveAssignedConnection` | Move assignment preserves state |
| `DoubleCloseIsSafe` | `close()` twice is safe |

**Total new integration tests: 24**

---

## Part 2: System Tests — TLS over TcpTransport

**New file:** `tests/system/test_system_tls_transport.cpp`

Add to `tests/system/CMakeLists.txt` `TEST_SYSTEM_SOURCES`.

Uses `ActorSystem` with `enable_network = true`, `PoolConfig::use_tls = true`,
`StaticDiscovery`, ephemeral ports (`tcp_port = 0`). All tests poll with
`assert_eventually` (5s deadline), no timing assumptions.

### Transport Lifecycle (4 tests)

| Test | Verifies |
|------|----------|
| `TlsTransportListenOnEphemeralPort` | System starts, transport non-null, endpoint has non-zero port |
| `TlsTransportIsConnectedFalseForUnknown` | `is_connected()` false for unknown |
| `TlsTransportCloseConnectionUnknownSafe` | `close_connection()` safe for unknown |
| `TwoTlsSystemsDifferentPorts` | Two systems start cleanly, shutdown cleanly |

### TLS Client↔Server Connect (3 tests)

| Test | Verifies |
|------|----------|
| `TlsConnectLoopback` | Client connects to server, `is_connected()` true |
| `TlsConnectReturnsConnection` | `transport->connect(endpoint)` returns non-null `ConnectionPtr` |
| `TlsConnectToSelf` | Self-connect fails gracefully |

### Encrypted Message Exchange (3 tests)

| Test | Verifies |
|------|----------|
| `SendReceiveOverTls` | Spawn actor on system B, send from A over TLS, actor receives |
| `MultipleMessagesOverTls` | N messages over TLS, all arrive, count matches |
| `LargeMessageOverTls` | > 4KB payload delivered correctly |

### Error & Edge Cases (3 tests)

| Test | Verifies |
|------|----------|
| `TlsConnectWrongPort` | Connect to port with no TLS listener → error/appropriate state |
| `TlsShutdownWhileConnected` | Shutdown with active TLS connection → clean, no hang/fd leak |
| `TlsPlaintextMismatch` | TLS client to plaintext listener → does not complete successfully |

**Total new system tests: 13**

---

## Total: 37 new tests (24 integration + 13 system)

## Files Changed

| File | Change |
|------|--------|
| `tests/integration/net/test_tls_connection.cpp` | Major extension (~24 new tests) |
| `tests/system/test_system_tls_transport.cpp` | New file (~13 system tests) |
| `tests/system/CMakeLists.txt` | Add `test_system_tls_transport.cpp` to sources |

## Design Constraints

- No timing assumptions — system tests poll with 5s deadline
- No exceptions, no RTTI
- Cert generation uses OpenSSL C API (already a system dependency)
- System tests use ephemeral ports (`tcp_port = 0`) to avoid port conflicts
- Integration tests use `socketpair()` for deterministic communication
- No shared mutable state between test cases
