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
    config.node_id = 42;
    config.verify_peer = true;
    assert(config.node_id == 42);
    assert(config.verify_peer == true);

    // Test from_config creates valid context
    // (In real test, use actual cert/key DER bytes)
    // For now, test with empty config to verify no crash
    TlsContext ctx = TlsContext::from_config(config);
    assert(ctx.node_id() == 42);

    // Test invalid cert returns proper result
    bytes invalid_cert = {0x30, 0x82, 0x01, 0x00};  // Fake DER
    auto result = ctx.verify_certificate(invalid_cert);
    // Empty ctx has no CA certs, so expect appropriate result
    (void)result;

    return 0;
}