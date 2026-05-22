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

#include <gtest/gtest.h>
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <cstring>
#include <string>

using namespace hpactor;
using namespace hpactor::net;

namespace {

StreamBuffer bytes_from(const char* str) {
    StreamBuffer buf;
    buf.append(reinterpret_cast<const uint8_t*>(str), strlen(str));
    return buf;
}

std::string body_to_string(const StreamBuffer& body) {
    return {body.begin(), body.end()};
}

} // namespace

TEST(HttpParserTest, ParseSimpleGet) {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    EXPECT_TRUE(received);
    EXPECT_EQ(captured.method, HttpMethod::GET);
    EXPECT_EQ(captured.path, "/");
    EXPECT_EQ(captured.http_major, 1);
    EXPECT_EQ(captured.http_minor, 1);
    EXPECT_GE(captured.headers.size(), 1u);
    EXPECT_TRUE(captured.body.empty());
    EXPECT_TRUE(parser.should_keep_alive());
}

TEST(HttpParserTest, ParsePostWithBody) {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from("POST /api/data HTTP/1.1\r\n"
                              "Content-Length: 5\r\n"
                              "\r\n"
                              "hello"));

    EXPECT_TRUE(received);
    EXPECT_EQ(captured.method, HttpMethod::POST);
    EXPECT_EQ(captured.path, "/api/data");
    EXPECT_EQ(captured.body.size(), 5u);
    EXPECT_EQ(body_to_string(captured.body), "hello");
}

TEST(HttpParserTest, ParseChunked) {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from("POST / HTTP/1.1\r\n"
                              "Transfer-Encoding: chunked\r\n"
                              "\r\n"
                              "5\r\nhello\r\n"
                              "0\r\n\r\n"));

    EXPECT_TRUE(received);
    EXPECT_EQ(captured.body.size(), 5u);
    EXPECT_EQ(body_to_string(captured.body), "hello");
}

TEST(HttpParserTest, KeepaliveDetection) {
    {
        HttpParser parser;
        parser.execute(bytes_from("GET / HTTP/1.1\r\n"
                                  "Host: localhost\r\n"
                                  "\r\n"));
        EXPECT_TRUE(parser.should_keep_alive());
    }

    {
        HttpParser parser;
        parser.execute(bytes_from("GET / HTTP/1.0\r\n"
                                  "Host: localhost\r\n"
                                  "\r\n"));
        EXPECT_FALSE(parser.should_keep_alive());
    }
}

TEST(HttpParserTest, ParseErrorMalformed) {
    HttpParser parser;
    bool error_received = false;

    parser.set_on_error([&](llhttp_errno_t /*err*/, const char* /*msg*/) {
        error_received = true;
    });

    parser.execute(bytes_from("NOTHTTP\r\n\r\n"));

    EXPECT_TRUE(error_received);
    EXPECT_EQ(parser.state(), HttpParseState::Error);
}

TEST(HttpParserTest, ParserResetAndReuse) {
    HttpParser parser;
    int count = 0;

    parser.set_on_message([&](HttpRequest&& /*req*/) { ++count; });

    parser.execute(bytes_from("GET /a HTTP/1.1\r\n\r\n"));
    EXPECT_EQ(count, 1);

    parser.reset();
    parser.execute(bytes_from("GET /b HTTP/1.1\r\n\r\n"));
    EXPECT_EQ(count, 2);
    EXPECT_EQ(parser.state(), HttpParseState::Complete);
}

TEST(HttpParserTest, IncrementalFeed) {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    const char* raw = "GET /test HTTP/1.1\r\nHost: example.com\r\n\r\n";
    size_t len = strlen(raw);

    for (size_t i = 0; i < len; i += 4) {
        size_t chunk = std::min(size_t(4), len - i);
        StreamBuffer chunk_buf;
        chunk_buf.append(reinterpret_cast<const uint8_t*>(raw + i), chunk);
        parser.execute(chunk_buf);
    }

    EXPECT_TRUE(received);
    EXPECT_EQ(captured.method, HttpMethod::GET);
    EXPECT_EQ(captured.path, "/test");
}

TEST(HttpParserTest, UpgradeDetection) {
    HttpParser parser;

    parser.execute(bytes_from("GET /ws HTTP/1.1\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: upgrade\r\n"
                              "\r\n"));

    EXPECT_TRUE(parser.upgrade_requested());
}
