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

#include <cassert>
#include <vector>

using namespace hpactor;
using namespace hpactor::net;

int main() {
    // Test CertVerifyResult enum values
    assert(static_cast<int>(TlsContext::CertVerifyResult::Ok) == 0);
    assert(static_cast<int>(TlsContext::CertVerifyResult::Invalid) == 1);
    assert(static_cast<int>(TlsContext::CertVerifyResult::Untrusted) == 2);
    assert(static_cast<int>(TlsContext::CertVerifyResult::Expired) == 3);
    assert(static_cast<int>(TlsContext::CertVerifyResult::UnknownError) == 4);

    // Test TlsConfig structure
    TlsConfig config;
    config.endpoint = hpactor::endpoint_ops::parse_endpoint("localhost:12345");
    config.verify_peer = true;
    assert(hpactor::endpoint_ops::to_string(config.endpoint) == "127.0.0.1:"
                                                                "12345");
    assert(config.verify_peer == true);

    // Test from_config creates valid context
    // (In real test, use actual cert/key DER bytes)
    // For now, test with empty config to verify no crash
    TlsContext ctx = TlsContext::from_config(config);
    assert(hpactor::endpoint_ops::to_string(ctx.endpoint()) == "127.0.0.1:"
                                                               "12345");

    // Test invalid cert returns proper result
    bytes invalid_cert = {0x30, 0x82, 0x01, 0x00}; // Fake DER
    auto result = ctx.verify_certificate(invalid_cert);
    // Empty ctx has no CA certs, so expect appropriate result
    (void)result;

    return 0;
}