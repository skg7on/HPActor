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

#pragma once

#include <hpactor/types/types.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor {

namespace net {

// Forward declarations
struct TlsConfig;
class TlsContext;

// -----------------------------------------------------------------------------
// TlsContext - TLS configuration and crypto operations
// -----------------------------------------------------------------------------
// Manages certificates, private keys, and provides crypto operations for
// the TLS handshake. Supports both filesystem-based and in-memory
// configuration.
// -----------------------------------------------------------------------------
class TlsContext {
  public:
    // Certificate verification result
    enum class CertVerifyResult {
        Ok,
        Invalid,
        Untrusted,
        Expired,
        UnknownError,
    };

    ~TlsContext();

    // Non-copyable
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;

    // Moveable
    TlsContext(TlsContext&&) noexcept;
    TlsContext& operator=(TlsContext&&) noexcept;

    // Create from filesystem paths
    // Expected structure:
    //   cert_dir/
    //     node_<node_id>.pem       - own certificate
    //     node_<node_id>_key.pem   - own private key
    //     ca.pem                   - trusted CA certificate
    //     remote/                  - trusted peer certificates
    static TlsContext
    from_filesystem(EndPoint endpoint, const std::string& cert_dir);

    // Create from in-memory configuration
    static TlsContext from_config(const TlsConfig& config);

    // Verify peer certificate against trusted CAs
    CertVerifyResult verify_certificate(const bytes& cert_der) const;

    // Sign data with own private key (for CertificateVerify)
    bytes sign_data(const bytes& data) const;

    // Decrypt pre_master_secret using own private key (RSA decryption)
    // Returns true on success, false on failure
    bool decrypt_pre_master_secret(const bytes& encrypted,
                                   bytes& pre_master_secret) const;

    // Get public key bytes from own certificate (for sending to peer)
    const bytes& public_key() const {
        return public_key_;
    }

    // Get own node ID
    EndPoint endpoint() const {
        return endpoint_;
    }

    // Get own certificate in DER format
    const bytes& certificate() const {
        return certificate_;
    }

  private:
    TlsContext();

    EndPoint endpoint_;
    bytes certificate_;
    bytes public_key_;
    bytes private_key_;

    // RSA key handle (OpenSSL)
    struct RSAKey;
    std::unique_ptr<RSAKey> rsa_key_;

    // Trusted CA certificates (for verification)
    std::vector<bytes> ca_certs_;

    // Peer certificates (for verification)
    std::vector<bytes> peer_certs_;
};

// Configuration for in-memory TLS setup
struct TlsConfig {
    bytes own_cert_der;
    bytes own_key_der;
    std::vector<bytes> ca_certs_der;
    EndPoint endpoint; // local endpoint for this node
    bool verify_peer = true;
};

} // namespace net
} // namespace hpactor