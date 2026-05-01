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

#include <hpactor/actor/typed_message.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

#include <cassert>
#include <cstring>
#include <string>

using namespace hpactor;
using namespace hpactor::net;

namespace {

StreamBuffer make_body(const char* str) {
    StreamBuffer body;
    body.append(reinterpret_cast<const uint8_t*>(str), strlen(str));
    return body;
}

} // namespace

// =============================================================================
// Test 9: Deserialize JSON content type — body preserved
// =============================================================================
static void test_deserialize_json_content_type() {
    HttpSerializer serializer;

    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = "/test";
    req.headers = {{"content-type", "application/json"}};
    req.body = make_body("{\"delta\": 5}");

    auto result = serializer.deserialize_request(req, TypeTag::User);
    assert(result.has_value());
    auto& msg = result.value();
    assert(msg.type_id() == TypeTag::User);
    // Body bytes preserved for actor to parse
    assert(msg.payload().size() > 0);
}

// =============================================================================
// Test 10: Deserialize protobuf content type — StreamBuffer pass through
// =============================================================================
static void test_deserialize_protobuf_content_type() {
    HttpSerializer serializer;

    uint8_t raw[] = {0x08, 0x05}; // protobuf: field 1 varint = 5
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = "/test";
    req.headers = {{"content-type", "application/x-protobuf"}};
    req.body.append(raw, 2);

    auto result = serializer.deserialize_request(req, TypeTag::User);
    assert(result.has_value());
    auto& msg = result.value();
    assert(msg.payload().size() == 2);
    assert(msg.payload().data()[0] == 0x08);
    assert(msg.payload().data()[1] == 0x05);
}

// =============================================================================
// Test 11: Serialize response — Accept application/json
// =============================================================================
static void test_serialize_response_accept_json() {
    HttpSerializer serializer;

    StreamBuffer payload = make_body("test-data");
    TypedMessage msg(TypeTag::User, payload);

    auto [body, content_type] = serializer.serialize_response(
        msg, "application/json");

    assert(content_type.find("application/json") != std::string::npos);
    // JSON wrapper includes data (hex-encoded payload inside JSON)
    assert(body.size() > 0);
}

// =============================================================================
// Test 12: Serialize with quality weights — higher quality wins
// =============================================================================
static void test_serialize_accept_quality_weights() {
    HttpSerializer serializer;

    StreamBuffer payload = make_body("data");
    TypedMessage msg(TypeTag::User, payload);

    // JSON has q=0.8, text/plain has q=0.5 → JSON should win
    auto [body, content_type] = serializer.serialize_response(
        msg, "text/plain;q=0.5, application/json;q=0.8");

    assert(content_type.find("application/json") != std::string::npos);
}

// =============================================================================
// Test 13: Serialize — no Accept header defaults to JSON
// =============================================================================
static void test_serialize_no_accept_default_json() {
    HttpSerializer serializer;

    StreamBuffer payload = make_body("data");
    TypedMessage msg(TypeTag::User, payload);

    auto [body, content_type] = serializer.serialize_response(msg, "");

    assert(content_type.find("application/json") != std::string::npos);
}

int main() {
    test_deserialize_json_content_type();
    test_deserialize_protobuf_content_type();
    test_serialize_response_accept_json();
    test_serialize_accept_quality_weights();
    test_serialize_no_accept_default_json();
    return 0;
}
