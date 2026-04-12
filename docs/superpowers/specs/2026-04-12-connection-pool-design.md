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

### RSA Key Transport

1. Client generates random 256-bit session key
2. Client RSA-encrypts session key with server's public key from certificate
3. Server RSA-decrypts to obtain session key
4. Both derive AES key + IV from session key using SHA-256 KDF

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

Round-robin across active connections for a given node:

```cpp
ConnectionPtr pool.get_connection() {
    if (active.empty()) return nullptr;
    auto conn = active[next_index_ % active.size()];
    next_index_++;
    return conn;
}
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
| Certificate invalid | Close connection, log error, don't reconnect |
| Certificate revoked | Close connection, alert via monitoring |
| Session key mismatch | Close connection, restart handshake |
| Connection pool exhausted | Return `error::mailbox_full` to sender |
| All reconnects failed | Mark node unreachable, return errors |

## OpenSSL Integration

### Required Operations

- `RSA_generate_key()` / `RSA_free()` — key generation
- `RSA_public_encrypt()` / `RSA_private_decrypt()` — session key exchange
- `X509_new()` / `X509_free()` — certificate parsing
- `X509_verify()` — certificate verification
- `SHA256()` — digest computation
- `AES_encrypt()` / `AES_decrypt()` — session encryption

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
    bool verify_certificate(const bytes& cert_der) const;

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
