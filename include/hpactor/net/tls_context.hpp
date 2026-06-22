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

struct TlsConfig;
class TlsContext;

/// \brief TLS configuration and cryptographic operations.
///
/// Manages certificates, private keys, and provides crypto operations for
/// the TLS handshake. Supports both filesystem-based and in-memory
/// configuration. Uses OpenSSL for RSA signing, verification, and
/// encryption.
///
/// \note Moveable but non-copyable. Owns OpenSSL key material — ensure
///       proper destruction ordering.
class TlsContext {
  public:
    /// \brief Certificate verification results.
    enum class CertVerifyResult {
        Ok,           ///< Certificate is valid and trusted.
        Invalid,      ///< Certificate is malformed.
        Untrusted,    ///< Certificate is not signed by a trusted CA.
        Expired,      ///< Certificate has expired.
        UnknownError, ///< Verification failed for an unknown reason.
    };

    ~TlsContext();

    /// \name Non-copyable, moveable
    /// @{
    TlsContext(const TlsContext&) = delete;
    TlsContext& operator=(const TlsContext&) = delete;
    TlsContext(TlsContext&&) noexcept;
    TlsContext& operator=(TlsContext&&) noexcept;
    /// @}

    /// \brief Create from a filesystem certificate directory.
    ///
    /// Expected structure:
    /// \code
    ///   cert_dir/
    ///     node_<node_id>.pem       - own certificate
    ///     node_<node_id>_key.pem   - own private key
    ///     ca.pem                   - trusted CA certificate
    ///     remote/                  - trusted peer certificates
    /// \endcode
    /// \param[in] endpoint Local node endpoint.
    /// \param[in] cert_dir Path to the certificate directory.
    /// \return Configured \c TlsContext.
    static TlsContext
    from_filesystem(EndPoint endpoint, const std::string& cert_dir);

    /// \brief Create from in-memory configuration.
    ///
    /// \param[in] config TLS configuration with certificate/key DER bytes.
    /// \return Configured \c TlsContext.
    static TlsContext from_config(const TlsConfig& config);

    /// \brief Verify a peer certificate against trusted CAs.
    ///
    /// \param[in] cert_der Peer certificate in DER format.
    /// \return Verification result code.
    CertVerifyResult verify_certificate(const StreamBuffer& cert_der) const;

    /// \brief Sign data with own private key (for CertificateVerify).
    ///
    /// \param[in] data Data to sign (typically a nonce).
    /// \return RSA signature bytes.
    StreamBuffer sign_data(const StreamBuffer& data) const;

    /// \brief Decrypt the pre-master secret using own private key.
    ///
    /// \param[in] encrypted RSA-encrypted pre-master secret.
    /// \param[out] pre_master_secret Receives the decrypted secret.
    /// \return \c true on success.
    bool decrypt_pre_master_secret(const StreamBuffer& encrypted,
                                   StreamBuffer& pre_master_secret) const;

    /// \brief Return the public key extracted from own certificate.
    ///
    /// \return Reference to the public key bytes.
    const StreamBuffer& public_key() const {
        return public_key_;
    }

    /// \brief Return the local endpoint this context is bound to.
    ///
    /// \return Local node endpoint.
    EndPoint endpoint() const {
        return endpoint_;
    }

    /// \brief Return own certificate in DER format.
    ///
    /// \return Reference to the certificate bytes.
    const StreamBuffer& certificate() const {
        return certificate_;
    }

  private:
    TlsContext();

    EndPoint endpoint_;
    StreamBuffer certificate_;
    StreamBuffer public_key_;
    StreamBuffer private_key_;

    struct RSAKey;
    std::unique_ptr<RSAKey> rsa_key_;
    std::vector<StreamBuffer> ca_certs_;
    std::vector<StreamBuffer> peer_certs_;
};

/// \brief In-memory TLS configuration for programmatic setup.
struct TlsConfig {
    /// \brief Own certificate in DER format.
    StreamBuffer own_cert_der;
    /// \brief Own private key in DER format.
    StreamBuffer own_key_der;
    /// \brief Trusted CA certificates in DER format.
    std::vector<StreamBuffer> ca_certs_der;
    /// \brief Local endpoint for this node.
    EndPoint endpoint;
    /// \brief Whether to verify peer certificates (default true).
    bool verify_peer = true;
};

} // namespace net
} // namespace hpactor
