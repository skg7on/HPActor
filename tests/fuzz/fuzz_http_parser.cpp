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

/// \file fuzz_http_parser.cpp
/// \brief Fuzz target for \c HttpParser::execute() — the streaming HTTP/1.1
///        parser that wraps llhttp for parsing request/response bytes.

#include "fuzz_harness.hpp"
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/net/http_parser.hpp>
#include <hpactor/net/http_serializer.hpp>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        StreamBuffer buf(d, d + s);

        // Test 1: Request-mode parse — single-shot feed
        {
            net::HttpParser parser(net::HttpParserMode::Request);
            bool complete = false;
            parser.set_on_message([&](net::HttpRequest&& req) {
                complete = true;
                // Exercise the deserialized request
                (void)req.method;
                (void)req.path.size();
                (void)req.headers.size();
                (void)req.body.size();
                (void)req.http_major;
                (void)req.http_minor;
                // Exercise content-type lookup
                auto ct = req.content_type();
                (void)ct;

                // Exercise HttpSerializer on the parsed request
                net::HttpSerializer serializer;
                auto result =
                    serializer.deserialize_request(req, TypeTag::MetricsRequestTag);
                (void)result;

                // Exercise serialize_response (exercises Accept header parsing
                // internally via negotiate_response_type)
                auto ac = req.header("accept");
                std::string accept_val = ac.value_or("application/json");
                TypedMessage dummy_msg(TypeTag::MetricsRequestTag, StreamBuffer());
                auto ser_result =
                    serializer.serialize_response(dummy_msg, accept_val);
                (void)ser_result;
            });
            parser.set_on_error([](llhttp_errno_t, const char*) {});

            size_t consumed = parser.execute(buf);
            (void)consumed;
            (void)complete;
        }

        // Test 2: Response-mode parse
        {
            net::HttpParser parser(net::HttpParserMode::Response);
            parser.set_on_response([](int status,
                                      const std::vector<net::HttpHeader>& headers,
                                      const StreamBuffer& body) {
                (void)status;
                (void)headers.size();
                (void)body.size();
            });
            parser.set_on_error([](llhttp_errno_t, const char*) {});
            parser.execute(buf);
        }

        // Test 3: Incremental feeding — split input at midpoint
        if (s > 1) {
            net::HttpParser parser(net::HttpParserMode::Request);
            parser.set_on_message([&](net::HttpRequest&&) {});
            parser.set_on_error([](llhttp_errno_t, const char*) {});
            size_t mid = s / 2;
            StreamBuffer first(d, d + mid);
            StreamBuffer second(d + mid, d + s);
            parser.execute(first);
            parser.execute(second);
        }

        // Test 4: Byte-at-a-time feeding — exercises state machine edge cases
        if (s > 0 && s <= 256) {
            net::HttpParser parser(net::HttpParserMode::Request);
            parser.set_on_message([&](net::HttpRequest&&) {});
            parser.set_on_error([](llhttp_errno_t, const char*) {});
            for (size_t i = 0; i < s; ++i) {
                StreamBuffer single(d + i, d + i + 1);
                parser.execute(single);
            }
        }
    });
}
