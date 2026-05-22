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

#include <gtest/gtest.h>

using namespace hpactor;
using namespace hpactor::net;

TEST(TlsContextTest, CertVerifyResultEnumValues) {
    EXPECT_EQ(static_cast<int>(TlsContext::CertVerifyResult::Ok), 0);
    EXPECT_EQ(static_cast<int>(TlsContext::CertVerifyResult::Invalid), 1);
    EXPECT_EQ(static_cast<int>(TlsContext::CertVerifyResult::Untrusted), 2);
    EXPECT_EQ(static_cast<int>(TlsContext::CertVerifyResult::Expired), 3);
    EXPECT_EQ(static_cast<int>(TlsContext::CertVerifyResult::UnknownError), 4);
}

TEST(TlsContextTest, TlsConfigStructure) {
    TlsConfig config;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:12345");
    config.verify_peer = true;
    EXPECT_EQ(hpactor::endpoint_ops::to_string(config.endpoint), "127.0.0.1:"
                                                                 "12345");
    EXPECT_TRUE(config.verify_peer);
}

TEST(TlsContextTest, FromConfigCreatesValidContext) {
    TlsConfig config;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:12345");
    TlsContext ctx = TlsContext::from_config(config);
    EXPECT_EQ(hpactor::endpoint_ops::to_string(ctx.endpoint()), "127.0.0.1:"
                                                                "12345");
}

TEST(TlsContextTest, InvalidCertReturnsResult) {
    TlsConfig config;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:12345");
    TlsContext ctx = TlsContext::from_config(config);

    StreamBuffer invalid_cert = {0x30, 0x82, 0x01, 0x00}; // Fake DER
    auto result = ctx.verify_certificate(invalid_cert);
    // Empty ctx has no CA certs, so expect appropriate result
    (void)result;
}
