# Phase 4: Connection Pool and TLS Handshake

## Overview

Phase 4 implements multi-connection pooling per remote node with TLS-like certificate-based mutual authentication for the HPActor distributed actor framework.

## Goals

- **Connection pooling**: Dynamic connection pool per remote node (0 to N connections)
- **Mutual authentication**: TLS-style certificate exchange with RSA key transport
- **Session encryption**: AES-256 session keys derived from RSA key exchange
- **Automatic reconnection**: Exponential backoff with configurable max attempts
- **Zero external dependencies for core**: OpenSSL for crypto operations

## Architecture

### Components

```
TcpTransport
├── ConnectionPool (per remote NodeId)
│   ├── Dynamic pool: grows on demand up to max_connections
│   ├── Background reconnection with exponential backoff
│   └── Round-robin connection selection
├── TlsContext (TLS configuration)
│   ├── Own certificate + private key
│   ├── Trusted CA certificates
│   └── Certificate verification settings
└── EventLoop (kqueue/epoll)

TlsConnection
├── TCP socket (non-blocking)
├── TLS state machine
│   ├── HandshakeState (client/server phases)
│   └── SessionState (encrypted mode)
└── Session key (AES-256)
```

### Directory Structure

```
src/net/
├── connection_pool.cpp      # ConnectionPool implementation
├── connection_pool.hpp
├── tls_context.cpp         # TlsContext implementation
├── tls_context.hpp
├── tls_connection.cpp      # TlsConnection implementation
├── tls_connection.hpp
├── frame.cpp               # (existing) Frame encoding
└── tcp_transport.cpp       # Updated to use pool + TLS
```

## TLS Handshake Protocol

### Message Types

```cpp
enum class TlsMessageType : uint8_t {
    ClientHello = 1,
    ServerHello = 2,
    Certificate = 3,
    CertificateVerify = 4,
    Finished = 5,
};
```

### Handshake Flow

```
Client                          Server
  |                               |
  |--- ClientHello -------------->|
  |    (node_id, client_certs)    |
  |                               |
  |<-- ServerHello ---------------|
  |    (node_id, server_certs)    |
  |                               |
  |<-- Certificate ---------------|
  |    (server X.509 cert chain)  |
  |                               |
  |--- Certificate -------------->|
  |    (client X.509 cert chain)  |
  |                               |
  |<-- CertificateVerify ---------|
  |    (nonce signed with         |
  |     server private key)       |
  |                               |
  |--- CertificateVerify --------->|
  |    (nonce signed with         |
  |     client private key)        |
  |                               |
  |--- Finished ----------------->|  (session_key encrypted)
  |                               |
  |<-- Finished ------------------|  (session_key encrypted)
  |                               |
  |====== ENCRYPTED MODE =========|
```

### RSA Key Transport (TLS 1.2-style)

1. Client generates random 48-byte **pre_master_secret**
2. Client RSA-encrypts pre_master_secret with server's public key from certificate
3. Server RSA-decrypts to obtain pre_master_secret
4. Both derive **master_secret** = PRF(pre_master_secret, "master secret", client_random + server_random)
5. Both derive **session keys** = PRF(master_secret, "key expansion", server_random + client_random)
   - AES-256 key for encryption
   - IV for CBC mode (or use AEAD like AES-GCM)

**Key derivation uses TLS PRF** (SHA-256 based):
```
master_secret = PRF(pre_master_secret, "master secret", client_random || server_random)
session_key = PRF(master_secret, "key expansion", server_random || client_random)
```

Both Finished messages contain verify_data = PRF(master_secret, "client/server finished", SHA-256(handshake_messages)), allowing each side to confirm the other derived the same keys.

### Certificate Format

X.509 DER-encoded certificates stored as PEM files:

```
Certificate:
  - Subject: CN=<node_id>
  - Issuer: CA certificate
  - Public key: RSA-2048
  - Signature: SHA-256 with RSA
```

## Connection Pool

### Pool Configuration

```cpp
struct PoolConfig {
    size_t min_connections = 1;      // Keep-alive connections
    size_t max_connections = 4;      // Upper bound per node
    size_t max_pending = 1000;       // Messages buffered during reconnect
    size_t max_attempts = 5;          // Reconnection attempts
    std::chrono::milliseconds initial_backoff{1000};
    std::chrono::milliseconds max_backoff{16000};
};
```

### Pool Behavior

| Event | Action |
|-------|--------|
| Send with empty pool | Create new connection, start TLS handshake |
| Connection closes | Mark unhealthy, start reconnect with backoff |
| Reconnect succeeds | Flush pending queue, return to active pool |
| Reconnect fails | Double backoff, retry up to max_attempts |
| All attempts exhausted | Return error to sender, stop pool |
| Pool at max_connections | Queue message (up to max_pending) |

### Connection Selection

Round-robin across active connections for a given node. Thread-safe via `std::atomic<size_t>`:

```cpp
class ConnectionPool {
    std::vector<ConnectionPtr> active_;
    std::atomic<size_t> next_index_{0};

public:
    ConnectionPtr get_connection() {
        if (active_.empty()) return nullptr;
        auto index = next_index_.fetch_add(1) % active_.size();
        return active_[index];
    }
};
```
```

## Data Flow

```
ActorSystem.send(remote_actor, message)
  → ActorProxy::send()
    → TcpTransport::send(actor_address, frame_bytes)
      → ConnectionPool::send()
        → TlsConnection::send(encrypted_payload)
          → EventLoop::write(fd, encrypted_data)

EventLoop::on_read(fd)
  → TlsConnection::handle_read()
    → TlsConnection::decrypt()
      → ConnectionPool::deliver(decrypted_frame)
        → ActorSystem::deliver(actor_address, message)
```

## File Formats

### Certificate Storage (Filesystem)

```
/etc/hpactor/certs/
├── node_<node_id>.pem       # Own certificate
├── node_<node_id>_key.pem   # Own private key (permissions: 600)
├── ca.pem                   # Trusted CA certificate
└── remote/
    ├── node_<id>.pem         # Trusted peer certificates
    └── node_<id>_fingerprint # SHA-256 of expected cert
```

### In-Memory Configuration

```cpp
struct TlsConfig {
    bytes own_cert_der;       // Own X.509 certificate (DER)
    bytes own_key_der;        // RSA private key (DER/PKCS#8)
    std::vector<bytes> ca_certs_der;  // Trusted CA certs
    bool verify_peer = true;
};

// Passed to TlsContext::create(config)
```

## Error Handling

| Error | Handling |
|-------|----------|
| TLS handshake timeout | Close connection, start reconnect |
| Certificate invalid (CertVerifyResult::Invalid) | Close connection, log error, don't reconnect |
| Certificate untrusted (CertVerifyResult::Untrusted) | Close connection, log error, don't reconnect |
| Certificate expired (CertVerifyResult::Expired) | Close connection, log error, don't reconnect |
| Session key mismatch | Close connection, restart handshake |
| Connection pool exhausted | Return `error::mailbox_full` to sender |
| All reconnects failed | Mark node unreachable, return errors |

## OpenSSL Integration

### Required Operations

- `EVP_PKEY_CTX_new()` / `EVP_PKEY_CTX_free()` — key generation context
- `EVP_PKEY_keygen_init()` / `EVP_PKEY_keygen()` — RSA key generation (OpenSSL 3.0 compatible)
- `RSA_public_encrypt()` / `RSA_private_decrypt()` — pre_master_secret exchange
- `d2i_X509()` / `X509_free()` — DER certificate parsing
- `X509_verify()` — certificate verification
- `EVP_MD_CTX_new()` / `EVP_MD_CTX_free()` — signing/verification context
- `EVP_DigestSign()` / `EVP_DigestVerify()` — for CertificateVerify signatures
- `EVP_CIPHER_CTX_new()` / `EVP_CIPHER_CTX_free()` — AES encryption context
- `EVP_CipherInit_ex()` / `EVP_CipherUpdate()` / `EVP_CipherFinal_ex()` — AES-CBC encryption

### Linkage

```cmake
find_package(OpenSSL REQUIRED)
target_link_libraries(hpactor_net PUBLIC OpenSSL::SSL OpenSSL::Crypto)
```

## Testing

### Unit Tests

- `test_connection_pool.cpp`: Pool creation, connection selection, backoff
- `test_tls_context.cpp`: Certificate loading, key parsing
- `test_tls_handshake.cpp`: Handshake state machine (mock socket)
- `test_frame.cpp`: Existing, ensure encryption compat

### Integration Tests

- `test_tls_connection_local.cpp`: Two TcpTransports on localhost
- `test_pool_reconnect.cpp`: Simulate connection drop, verify reconnect

## API Changes

### TcpTransport

```cpp
class TcpTransport : public Transport {
public:
    TcpTransport(NodeId node_id, const TlsConfig& tls_config,
                 const PoolConfig& pool_config);

    // ... existing interface unchanged ...
};
```

### ConnectionPool (new public interface)

```cpp
class ConnectionPool {
public:
    ConnectionPool(NodeId remote_node, const PoolConfig& config,
                   TlsContext* tls_context, EventLoop* loop);

    // Send to remote node (uses pool)
    void send(const ActorAddress& target, const bytes& encoded);

    // Check connectivity
    bool is_connected() const;

    // Stats
    size_t active_connections() const;
    size_t pending_messages() const;

    // Graceful shutdown: drain pending messages, close all connections
    // Returns number of messages that could not be sent
    size_t drain();

    // Stop accepting new messages and close connections immediately
    void abort();
};
```

### TlsContext (new public interface)

```cpp
class TlsContext {
public:
    // Create from filesystem paths
    static TlsContext from_filesystem(const std::string& cert_dir);

    // Create from in-memory config
    static TlsContext from_config(const TlsConfig& config);

    // Verify peer certificate
    enum class CertVerifyResult {
        Ok,              // Certificate is valid
        Invalid,         // Malformed, wrong signature, unsupported extensions
        Untrusted,       // Not signed by a trusted CA
        Expired,         // Certificate validity period has passed
        UnknownError
    };
    CertVerifyResult verify_certificate(const bytes& cert_der) const;

    // Sign challenge (for CertificateVerify)
    bytes sign_challenge(const bytes& challenge) const;

    // Decrypt session key
    bool decrypt_session_key(const bytes& encrypted,
                             bytes& session_key) const;

    // Get public key for encryption
    const bytes& public_key() const;
};
```

## Open Questions

1. **Session key rotation**: Should sessions expire and renegotiate after N bytes or T time?
2. **Certificate revocation**: CRL distribution or OCSP stapling for rev check?
3. **Connection prioritization**: High-priority messages on dedicated connection?

These can be addressed in Phase 5 if needed.

## Implementation Order

1. `TlsContext` — certificate loading, RSA operations, session key crypto
2. `TlsConnection` — TLS state machine, handshake protocol
3. `ConnectionPool` — dynamic pool management, reconnection logic
4. `TcpTransport` updates — integrate pool + TLS
5. Tests and integration tests
