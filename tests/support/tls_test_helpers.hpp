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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/net/tls_context.hpp>

#include <cstring>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>

namespace hpactor::test {

struct TestCerts {
    adt::StreamBuffer cert_der;
    adt::StreamBuffer key_der;
    adt::StreamBuffer pub_key_der;
};

// Generate a self-signed RSA 2048-bit certificate for testing.
// The returned cert/key are in DER format, suitable for TlsConfig.
inline TestCerts generate_test_certs(const char* common_name = "hpactor-test") {
    TestCerts certs;

    // 1. Generate RSA key pair
    EVP_PKEY* pkey = EVP_PKEY_new();
    EVP_PKEY_CTX* pctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY_keygen_init(pctx);
    EVP_PKEY_CTX_set_rsa_keygen_bits(pctx, 2048);
    EVP_PKEY_keygen(pctx, &pkey);
    EVP_PKEY_CTX_free(pctx);

    // 2. Create self-signed X509 certificate
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year
    X509_set_pubkey(x509, pkey);

    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               reinterpret_cast<const unsigned char*>(common_name),
                               -1, -1, 0);
    X509_set_subject_name(x509, name);
    X509_set_issuer_name(x509, name);
    X509_NAME_free(name);

    X509_sign(x509, pkey, EVP_sha256());

    // 3. Extract DER-encoded certificate
    int cert_len = i2d_X509(x509, nullptr);
    certs.cert_der.resize(static_cast<size_t>(cert_len));
    unsigned char* cert_ptr = certs.cert_der.data();
    i2d_X509(x509, &cert_ptr);

    // 4. Extract DER-encoded private key (PKCS#8)
    int key_len = i2d_PrivateKey(pkey, nullptr);
    certs.key_der.resize(static_cast<size_t>(key_len));
    unsigned char* key_ptr = certs.key_der.data();
    i2d_PrivateKey(pkey, &key_ptr);

    // 5. Extract DER-encoded public key (SubjectPublicKeyInfo)
    int pub_len = i2d_PUBKEY(pkey, nullptr);
    certs.pub_key_der.resize(static_cast<size_t>(pub_len));
    unsigned char* pub_ptr = certs.pub_key_der.data();
    i2d_PUBKEY(pkey, &pub_ptr);

    X509_free(x509);
    EVP_PKEY_free(pkey);

    return certs;
}

// Build a TlsContext from TestCerts for use in tests.
inline net::TlsContext make_tls_context_from_certs(const TestCerts& certs,
                                                    uint16_t port = 12345) {
    net::TlsConfig config;
    config.endpoint =
        hpactor::endpoint_ops::parse_endpoint("127.0.0.1:" + std::to_string(port));
    config.own_cert_der = certs.cert_der;
    config.own_key_der = certs.key_der;
    return net::TlsContext::from_config(config);
}

} // namespace hpactor::test
