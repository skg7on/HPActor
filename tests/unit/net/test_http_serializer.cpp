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
#include <hpactor/msg/typed_message.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/types/types.hpp>

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

TEST(HttpSerializerTest, DeserializeJsonContentType) {
    HttpSerializer serializer;

    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = "/test";
    req.headers = {{"content-type", "application/json"}};
    req.body = make_body("{\"delta\": 5}");

    auto result = serializer.deserialize_request(req, TypeTag::User);
    EXPECT_TRUE(result.has_value());
    auto& msg = result.value();
    EXPECT_EQ(msg.type_id(), TypeTag::User);
    EXPECT_GT(msg.payload().size(), 0u);
}

TEST(HttpSerializerTest, DeserializeProtobufContentType) {
    HttpSerializer serializer;

    uint8_t raw[] = {0x08, 0x05};
    HttpRequest req;
    req.method = HttpMethod::POST;
    req.path = "/test";
    req.headers = {{"content-type", "application/x-protobuf"}};
    req.body.append(raw, 2);

    auto result = serializer.deserialize_request(req, TypeTag::User);
    EXPECT_TRUE(result.has_value());
    auto& msg = result.value();
    EXPECT_EQ(msg.payload().size(), 2u);
    EXPECT_EQ(msg.payload().data()[0], 0x08);
    EXPECT_EQ(msg.payload().data()[1], 0x05);
}

TEST(HttpSerializerTest, SerializeResponseAcceptJson) {
    HttpSerializer serializer;

    StreamBuffer payload = make_body("test-data");
    TypedMessage msg(TypeTag::User, payload);

    auto [body, content_type] = serializer.serialize_response(msg, "application"
                                                                   "/json");

    EXPECT_NE(content_type.find("application/json"), std::string::npos);
    EXPECT_GT(body.size(), 0u);
}

TEST(HttpSerializerTest, SerializeAcceptQualityWeights) {
    HttpSerializer serializer;

    StreamBuffer payload = make_body("data");
    TypedMessage msg(TypeTag::User, payload);

    auto [body, content_type] = serializer.serialize_response(msg, "text/"
                                                                   "plain;q=0."
                                                                   "5, "
                                                                   "application"
                                                                   "/json;q=0."
                                                                   "8");

    EXPECT_NE(content_type.find("application/json"), std::string::npos);
}

TEST(HttpSerializerTest, SerializeRejectMalformedQuality) {
    // Malformed quality values must not crash (no exceptions in HPActor).
    // std::stof("notanumber") throws → std::terminate under -fno-exceptions.
    // The fix uses strtof which returns 0.0 on parse failure and we
    // clamp to the RFC 7231 default quality of 1.0.
    HttpSerializer serializer;

    StreamBuffer payload = make_body("data");
    TypedMessage msg(TypeTag::User, payload);

    // q=notanumber — would crash with std::stof, should fall back to 1.0
    auto [body1, ct1] =
        serializer.serialize_response(msg, "text/plain;q=notanumber");
    EXPECT_NE(ct1.find("text/plain"), std::string::npos);

    // q=999999999999999999999 — overflows float range, would crash with
    // std::stof
    auto [body2, ct2] =
        serializer.serialize_response(msg, "text/plain;q=999999999999999999999");
    EXPECT_NE(ct2.find("text/plain"), std::string::npos);

    // q= (empty quality value) — invalid parse, must not crash
    auto [body3, ct3] = serializer.serialize_response(msg, "text/plain;q=");
    EXPECT_NE(ct3.find("text/plain"), std::string::npos);
}

TEST(HttpSerializerTest, SerializeNoAcceptDefaultJson) {
    HttpSerializer serializer;

    StreamBuffer payload = make_body("data");
    TypedMessage msg(TypeTag::User, payload);

    auto [body, content_type] = serializer.serialize_response(msg, "");

    EXPECT_NE(content_type.find("application/json"), std::string::npos);
}
