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

/// \file test_fuzz_regression_http.cpp
/// \brief Regression tests for HTTP parser fuzz findings.

#include <gtest/gtest.h>
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_serializer.hpp>

using namespace hpactor;

namespace {

StreamBuffer buf_from(const char* s) {
    return StreamBuffer(reinterpret_cast<const uint8_t*>(s),
                        reinterpret_cast<const uint8_t*>(s) + std::strlen(s));
}

} // namespace

TEST(FuzzRegressionHttp, EmptyInput) {
    net::HttpParser parser(net::HttpParserMode::Request);
    parser.set_on_message([](net::HttpRequest&&) {});
    parser.set_on_error([](llhttp_errno_t, const char*) {});
    StreamBuffer empty;
    size_t consumed = parser.execute(empty);
    EXPECT_EQ(consumed, 0u);
}

TEST(FuzzRegressionHttp, BareLineFeed) {
    // HTTP with bare LF instead of CRLF — llhttp may reject this.
    // The contract is: parser must not crash.
    net::HttpParser parser(net::HttpParserMode::Request);
    bool had_message = false;
    parser.set_on_message([&](net::HttpRequest&&) { had_message = true; });
    parser.set_on_error([&](llhttp_errno_t, const char*) {
        // Error is acceptable — bare LF is not standards-compliant
    });
    StreamBuffer buf = buf_from("GET / HTTP/1.1\nHost: localhost\n\n");
    parser.execute(buf);
    // Critical invariant: no crash. Either outcome is fine.
    EXPECT_TRUE(true);
}

TEST(FuzzRegressionHttp, BinaryGarbage) {
    // Binary non-UTF-8 input — parser must not crash
    net::HttpParser parser(net::HttpParserMode::Request);
    bool errored = false;
    parser.set_on_message([](net::HttpRequest&&) {});
    parser.set_on_error([&](llhttp_errno_t, const char*) { errored = true; });
    const uint8_t garbage[] = {0x00, 0x01, 0x02, 0xFF, 0xFE};
    StreamBuffer buf(garbage, garbage + 5);
    parser.execute(buf);
    // Either a message or an error is fine — just no crash
    EXPECT_TRUE(true);
}

TEST(FuzzRegressionHttp, ValidGetRoot) {
    net::HttpParser parser(net::HttpParserMode::Request);
    bool complete = false;
    parser.set_on_message([&](net::HttpRequest&& req) {
        complete = true;
        EXPECT_EQ(req.method, net::HttpMethod::GET);
        EXPECT_EQ(req.path, "/");
        EXPECT_EQ(req.http_major, 1);
        EXPECT_EQ(req.http_minor, 1);
    });
    parser.set_on_error([](llhttp_errno_t, const char*) {
        FAIL() << "Valid GET should not error";
    });
    StreamBuffer buf = buf_from("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n");
    parser.execute(buf);
    EXPECT_TRUE(complete);
}

TEST(FuzzRegressionHttp, PostWithBody) {
    net::HttpParser parser(net::HttpParserMode::Request);
    parser.set_on_message([&](net::HttpRequest&& req) {
        EXPECT_EQ(req.method, net::HttpMethod::POST);
        EXPECT_EQ(req.path, "/api");
        // Body should be available
        (void)req.body.size();
    });
    parser.set_on_error([](llhttp_errno_t, const char*) {
        FAIL() << "Valid POST should not error";
    });
    StreamBuffer buf = buf_from("POST /api HTTP/1.1\r\n"
                                "Content-Type: text/plain\r\n"
                                "Content-Length: 5\r\n"
                                "\r\n"
                                "hello");
    parser.execute(buf);
}

TEST(FuzzRegressionHttp, IncrementalFeeding) {
    // Split a valid request across multiple execute() calls
    net::HttpParser parser(net::HttpParserMode::Request);
    bool complete = false;
    parser.set_on_message([&](net::HttpRequest&&) { complete = true; });
    parser.set_on_error([](llhttp_errno_t, const char*) {
        FAIL() << "Incremental feed should not error";
    });

    const char* req = "GET /foo HTTP/1.1\r\nHost: example.com\r\n\r\n";
    for (size_t i = 0; req[i]; ++i) {
        const uint8_t* p = reinterpret_cast<const uint8_t*>(&req[i]);
        StreamBuffer single(p, p + 1);
        parser.execute(single);
    }
    EXPECT_TRUE(complete);
}

TEST(FuzzRegressionHttp, AcceptHeaderParsing) {
    // Exercise Accept header parsing through the public serialize_response API
    net::HttpSerializer serializer;
    StreamBuffer empty_payload;
    TypedMessage msg(TypeTag::MetricsRequestTag, empty_payload);
    // Normal Accept header
    auto result = serializer.serialize_response(
        msg, "text/html, application/json;q=0.8, */*;q=0.1");
    EXPECT_FALSE(result.first.empty());
    EXPECT_FALSE(result.second.empty());

    // Empty Accept header
    auto result2 = serializer.serialize_response(msg, "");
    EXPECT_FALSE(result2.first.empty());
    // Should default to JSON
    EXPECT_EQ(result2.second, "application/json; charset=utf-8");
}

TEST(FuzzRegressionHttp, ResponseMode) {
    net::HttpParser parser(net::HttpParserMode::Response);
    bool complete = false;
    parser.set_on_response([&](int status, const std::vector<net::HttpHeader>& headers,
                               const StreamBuffer& body) {
        complete = true;
        EXPECT_EQ(status, 200);
        EXPECT_FALSE(headers.empty());
        (void)body.size();
    });
    parser.set_on_error([](llhttp_errno_t, const char*) {
        FAIL() << "Valid response should not error";
    });
    StreamBuffer buf = buf_from("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nOK");
    parser.execute(buf);
    EXPECT_TRUE(complete);
}
