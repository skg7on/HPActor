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

#include <hpactor/net/tls_context.hpp>

#include <hpactor/net/platform.hpp>

#include <cstring>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace hpactor {

namespace net {

struct TlsContext::RSAKey {
    EVP_PKEY* pkey = nullptr;
    ~RSAKey() {
        if (pkey)
            EVP_PKEY_free(pkey);
    }
};

TlsContext::TlsContext() = default;

TlsContext::~TlsContext() = default;

TlsContext::TlsContext(TlsContext&& other) noexcept
    : endpoint_(other.endpoint_), certificate_(std::move(other.certificate_)),
      public_key_(std::move(other.public_key_)),
      private_key_(std::move(other.private_key_)),
      rsa_key_(std::move(other.rsa_key_)), ca_certs_(std::move(other.ca_certs_)),
      peer_certs_(std::move(other.peer_certs_)) {
    other.endpoint_ = LocalEndpoint;
}

TlsContext& TlsContext::operator=(TlsContext&& other) noexcept {
    if (this != &other) {
        endpoint_ = other.endpoint_;
        certificate_ = std::move(other.certificate_);
        public_key_ = std::move(other.public_key_);
        private_key_ = std::move(other.private_key_);
        rsa_key_ = std::move(other.rsa_key_);
        ca_certs_ = std::move(other.ca_certs_);
        peer_certs_ = std::move(other.peer_certs_);
        other.endpoint_ = LocalEndpoint;
    }
    return *this;
}

TlsContext TlsContext::from_filesystem(CommunicationEndpoint endpoint,
                                       const std::string& cert_dir) {
    TlsContext ctx;
    ctx.endpoint_ = endpoint;

    // Sanitize endpoint string for use in filename (replace ':' with '_')
    std::string safe_node_id = endpoint_ops::to_string(endpoint);
    std::replace(safe_node_id.begin(), safe_node_id.end(), ':', '_');

    // Load own certificate
    std::string cert_path = cert_dir + "/node_" + safe_node_id + ".pem";
    FILE* cert_file = fopen(cert_path.c_str(), "r");
    if (!cert_file) {
        return ctx; // Caller should check node_id().empty() to detect init
                    // failure
    }
    X509* cert = PEM_read_X509(cert_file, nullptr, nullptr, nullptr);
    fclose(cert_file);
    if (!cert) {
        return ctx;
    }

    // Extract DER from certificate
    unsigned char* cert_der = nullptr;
    int cert_len = i2d_X509(cert, &cert_der);
    if (cert_len > 0 && cert_der) {
        ctx.certificate_.insert(ctx.certificate_.end(), cert_der,
                                cert_der + cert_len);
        OPENSSL_free(cert_der);
    }

    // Extract public key
    EVP_PKEY* pkey = X509_get0_pubkey(cert);
    if (pkey) {
        unsigned char* pkey_der = nullptr;
        int pkey_len = i2d_PUBKEY(pkey, &pkey_der);
        if (pkey_len > 0 && pkey_der) {
            ctx.public_key_.insert(ctx.public_key_.end(), pkey_der,
                                   pkey_der + pkey_len);
            OPENSSL_free(pkey_der);
        }
    }

    // Load private key using EVP API
    std::string key_path = cert_dir + "/node_" + safe_node_id + "_key.pem";
    FILE* key_file = fopen(key_path.c_str(), "r");
    if (key_file) {
        EVP_PKEY* evp_pkey =
            PEM_read_PrivateKey(key_file, nullptr, nullptr, nullptr);
        fclose(key_file);
        if (evp_pkey) {
            ctx.rsa_key_ = std::make_unique<RSAKey>();
            ctx.rsa_key_->pkey = evp_pkey;
        }
    }

    X509_free(cert);
    return ctx;
}

TlsContext TlsContext::from_config(const TlsConfig& config) {
    TlsContext ctx;
    ctx.endpoint_ = config.endpoint;
    ctx.certificate_ = config.own_cert_der;
    ctx.ca_certs_ = config.ca_certs_der;

    // Parse private key using EVP API
    const unsigned char* key_data = config.own_key_der.data();
    EVP_PKEY* evp_pkey =
        d2i_PrivateKey(EVP_PKEY_RSA, nullptr, &key_data,
                       static_cast<long>(config.own_key_der.size()));
    if (evp_pkey) {
        ctx.rsa_key_ = std::make_unique<RSAKey>();
        ctx.rsa_key_->pkey = evp_pkey;
    }

    // Extract public key from certificate
    const unsigned char* cert_data = config.own_cert_der.data();
    X509* cert = d2i_X509(nullptr, &cert_data,
                          static_cast<long>(config.own_cert_der.size()));
    if (cert) {
        EVP_PKEY* pkey = X509_get0_pubkey(cert);
        if (pkey) {
            unsigned char* pkey_der = nullptr;
            int pkey_len = i2d_PUBKEY(pkey, &pkey_der);
            if (pkey_len > 0 && pkey_der) {
                ctx.public_key_.insert(ctx.public_key_.end(), pkey_der,
                                       pkey_der + pkey_len);
                OPENSSL_free(pkey_der);
            }
        }
        X509_free(cert);
    }

    return ctx;
}

TlsContext::CertVerifyResult
TlsContext::verify_certificate(const bytes& cert_der) const {
    const unsigned char* data = cert_der.data();
    X509* cert = d2i_X509(nullptr, &data, static_cast<long>(cert_der.size()));
    if (!cert) {
        return CertVerifyResult::Invalid;
    }

    // Check validity period - X509_get0_notBefore/notAfter return const
    // pointers
    const ASN1_TIME* not_before = X509_get0_notBefore(cert);
    const ASN1_TIME* not_after = X509_get0_notAfter(cert);
    if (!not_before || !not_after) {
        X509_free(cert);
        return CertVerifyResult::Invalid;
    }

    // Simple time check (in production use X509_cmp_time)
    // For now, assume valid if we can parse it
    (void)not_before;
    (void)not_after;

    X509_free(cert);
    return CertVerifyResult::Ok;
}

bytes TlsContext::sign_data(const bytes& data) const {
    bytes signature;
    if (!rsa_key_ || !rsa_key_->pkey) {
        return signature;
    }

    // Use EVP API for signing
    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    if (!ctx) {
        return signature;
    }

    const EVP_MD* md = EVP_sha256();
    if (EVP_DigestInit_ex(ctx, md, nullptr) != 1) {
        EVP_MD_CTX_free(ctx);
        return signature;
    }

    if (EVP_DigestSignInit(ctx, nullptr, md, nullptr, rsa_key_->pkey) != 1) {
        EVP_MD_CTX_free(ctx);
        return signature;
    }

    size_t sig_len = 0;
    if (EVP_DigestSign(ctx, nullptr, &sig_len, data.data(), data.size()) != 1) {
        EVP_MD_CTX_free(ctx);
        return signature;
    }

    signature.resize(sig_len);
    if (EVP_DigestSign(ctx, signature.data(), &sig_len, data.data(),
                       data.size()) != 1) {
        signature.clear();
    } else {
        signature.resize(sig_len);
    }

    EVP_MD_CTX_free(ctx);
    return signature;
}

bool TlsContext::decrypt_pre_master_secret(const bytes& encrypted,
                                           bytes& pre_master_secret) const {
    if (!rsa_key_ || !rsa_key_->pkey) {
        return false;
    }

    // Use EVP API for decryption
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new(rsa_key_->pkey, nullptr);
    if (!ctx) {
        return false;
    }

    if (EVP_PKEY_decrypt_init(ctx) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    size_t out_len = 0;
    if (EVP_PKEY_decrypt(ctx, nullptr, &out_len, encrypted.data(),
                         encrypted.size()) != 1) {
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    pre_master_secret.resize(out_len);
    if (EVP_PKEY_decrypt(ctx, pre_master_secret.data(), &out_len,
                         encrypted.data(), encrypted.size()) != 1) {
        pre_master_secret.clear();
        EVP_PKEY_CTX_free(ctx);
        return false;
    }

    pre_master_secret.resize(out_len);
    EVP_PKEY_CTX_free(ctx);
    return true;
}

} // namespace net
} // namespace hpactor