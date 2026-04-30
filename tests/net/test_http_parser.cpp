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

#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <cassert>
#include <cstring>
#include <span>
#include <string>

using namespace hpactor;
using namespace hpactor::net;

namespace {

std::span<const uint8_t> bytes_from(const char* str) {
    return {reinterpret_cast<const uint8_t*>(str), strlen(str)};
}

std::string body_to_string(const bytes& body) {
    return {body.begin(), body.end()};
}

} // namespace

// =============================================================================
// Test 1: Parse simple GET request
// =============================================================================
static void test_parse_simple_get() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from("GET / HTTP/1.1\r\nHost: localhost\r\n\r\n"));

    assert(received);
    assert(captured.method == HttpMethod::GET);
    assert(captured.path == "/");
    assert(captured.http_major == 1);
    assert(captured.http_minor == 1);
    assert(captured.headers.size() >= 1);
    assert(captured.body.empty());
    assert(parser.should_keep_alive());
}

// =============================================================================
// Test 2: Parse POST with Content-Length body
// =============================================================================
static void test_parse_post_with_body() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from(
        "POST /api/data HTTP/1.1\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "hello"));

    assert(received);
    assert(captured.method == HttpMethod::POST);
    assert(captured.path == "/api/data");
    assert(captured.body.size() == 5);
    assert(body_to_string(captured.body) == "hello");
}

// =============================================================================
// Test 3: Parse chunked transfer encoding
// =============================================================================
static void test_parse_chunked() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    parser.execute(bytes_from(
        "POST / HTTP/1.1\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "5\r\nhello\r\n"
        "0\r\n\r\n"));

    assert(received);
    assert(captured.body.size() == 5);
    assert(body_to_string(captured.body) == "hello");
}

// =============================================================================
// Test 4: Keepalive detection — HTTP/1.1 defaults to keep-alive
// =============================================================================
static void test_parse_keepalive_detection() {
    // HTTP/1.1 defaults to keep-alive (no Connection header needed)
    {
        HttpParser parser;
        parser.execute(bytes_from(
            "GET / HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "\r\n"));
        assert(parser.should_keep_alive());
    }

    // HTTP/1.0 defaults to close (no Connection header)
    {
        HttpParser parser;
        parser.execute(bytes_from(
            "GET / HTTP/1.0\r\n"
            "Host: localhost\r\n"
            "\r\n"));
        assert(!parser.should_keep_alive());
    }
}

// =============================================================================
// Test 5: Parse error on malformed HTTP
// =============================================================================
static void test_parse_error_malformed() {
    HttpParser parser;
    bool error_received = false;

    parser.set_on_error([&](llhttp_errno_t /*err*/, const char* /*msg*/) {
        error_received = true;
    });

    parser.execute(bytes_from("NOTHTTP\r\n\r\n"));

    assert(error_received);
    assert(parser.state() == HttpParseState::Error);
}

// =============================================================================
// Test 6: Parser reset and reuse (keep-alive simulation)
// =============================================================================
static void test_parser_reset_and_reuse() {
    HttpParser parser;
    int count = 0;

    parser.set_on_message([&](HttpRequest&& /*req*/) { ++count; });

    // Parse first request
    parser.execute(bytes_from("GET /a HTTP/1.1\r\n\r\n"));
    assert(count == 1);

    // Reset and parse second request on same connection
    parser.reset();
    parser.execute(bytes_from("GET /b HTTP/1.1\r\n\r\n"));
    assert(count == 2);
    assert(parser.state() == HttpParseState::Complete);
}

// =============================================================================
// Test 7: Incremental feed (simulate TCP chunk delivery)
// =============================================================================
static void test_parse_incremental_feed() {
    HttpParser parser;
    HttpRequest captured;
    bool received = false;

    parser.set_on_message([&](HttpRequest&& req) {
        captured = std::move(req);
        received = true;
    });

    const char* raw = "GET /test HTTP/1.1\r\nHost: example.com\r\n\r\n";
    size_t len = strlen(raw);

    // Feed 4 bytes at a time
    for (size_t i = 0; i < len; i += 4) {
        size_t chunk = std::min(size_t(4), len - i);
        std::span<const uint8_t> data(
            reinterpret_cast<const uint8_t*>(raw + i), chunk);
        parser.execute(data);
    }

    assert(received);
    assert(captured.method == HttpMethod::GET);
    assert(captured.path == "/test");
}

// =============================================================================
// Test 8: Upgrade detection (WebSocket upgrade header)
// =============================================================================
static void test_parse_upgrade_detection() {
    HttpParser parser;

    parser.execute(bytes_from(
        "GET /ws HTTP/1.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: upgrade\r\n"
        "\r\n"));

    assert(parser.upgrade_requested());
}

int main() {
    test_parse_simple_get();
    test_parse_post_with_body();
    test_parse_chunked();
    test_parse_keepalive_detection();
    test_parse_error_malformed();
    test_parser_reset_and_reuse();
    test_parse_incremental_feed();
    test_parse_upgrade_detection();
    return 0;
}
