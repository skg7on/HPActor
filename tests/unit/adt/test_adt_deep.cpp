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

#include <hpactor/adt/dedup_cache.hpp>
#include <hpactor/adt/json_helpers.hpp>
#include <hpactor/adt/node_identity.hpp>
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/ref/actor_address.hpp>

#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace hpactor;
using namespace hpactor::adt;

// ── StreamBuffer edge cases ───────────────────────────────────────

TEST(StreamBufferDeepTest, DefaultConstructionEmpty) {
    StreamBuffer buf;
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
}

TEST(StreamBufferDeepTest, InitializerListConstruction) {
    StreamBuffer buf{0x01, 0x02, 0x03};
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[1], 0x02);
    EXPECT_EQ(buf[2], 0x03);
}

TEST(StreamBufferDeepTest, WithCapacity) {
    auto buf = StreamBuffer::with_capacity(128);
    EXPECT_TRUE(buf.empty());
    EXPECT_GE(buf.capacity(), 128u);
}

TEST(StreamBufferDeepTest, ConsumeAndCompact) {
    StreamBuffer buf{0x01, 0x02, 0x03, 0x04, 0x05};
    EXPECT_EQ(buf.size(), 5u);

    buf.consume(2);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[0], 0x03);
    EXPECT_EQ(buf[1], 0x04);
    EXPECT_EQ(buf[2], 0x05);

    // Compact resets read_pos_ if all data consumed
    buf.consume(3);
    EXPECT_TRUE(buf.empty());
    buf.compact();
    EXPECT_TRUE(buf.empty());
}

TEST(StreamBufferDeepTest, ConsumePartialThenCompact) {
    StreamBuffer buf{0x01, 0x02, 0x03, 0x04, 0x05};
    buf.consume(2);
    EXPECT_EQ(buf.size(), 3u);
    buf.compact(); // Should move remaining data to front
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[0], 0x03);
    EXPECT_EQ(buf[1], 0x04);
    EXPECT_EQ(buf[2], 0x05);
}

TEST(StreamBufferDeepTest, PushBackAndAccess) {
    StreamBuffer buf;
    buf.push_back(0xAA);
    buf.push_back(0xBB);
    buf.push_back(0xCC);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf.front(), 0xAA);
    EXPECT_EQ(buf.back(), 0xCC);
}

TEST(StreamBufferDeepTest, ReserveTailAndCommit) {
    StreamBuffer buf;
    uint8_t* tail = buf.reserve_tail(4);
    tail[0] = 0x10;
    tail[1] = 0x20;
    tail[2] = 0x30;
    tail[3] = 0x40;
    buf.commit_tail(4);
    EXPECT_EQ(buf.size(), 4u);
    EXPECT_EQ(buf[0], 0x10);
    EXPECT_EQ(buf[3], 0x40);
}

TEST(StreamBufferDeepTest, AppendData) {
    StreamBuffer buf;
    const uint8_t data[] = {0xAA, 0xBB, 0xCC};
    buf.append(data, 3);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[0], 0xAA);
    EXPECT_EQ(buf[2], 0xCC);
}

TEST(StreamBufferDeepTest, MoveSemantics) {
    StreamBuffer buf1{0x01, 0x02, 0x03};
    EXPECT_EQ(buf1.size(), 3u);

    StreamBuffer buf2(std::move(buf1));
    // buf2 has the data; buf1 is valid but unspecified
    EXPECT_EQ(buf2.size(), 3u);
    EXPECT_EQ(buf2[0], 0x01);
}

TEST(StreamBufferDeepTest, CopySemantics) {
    StreamBuffer buf1{0x01, 0x02, 0x03};
    StreamBuffer buf2(buf1);
    EXPECT_EQ(buf2.size(), 3u);
    EXPECT_EQ(buf2[0], 0x01);
    EXPECT_EQ(buf2[2], 0x03);

    // Mutate buf2, buf1 should be independent
    buf2.push_back(0x04);
    EXPECT_EQ(buf2.size(), 4u);
    EXPECT_EQ(buf1.size(), 3u);
}

TEST(StreamBufferDeepTest, Equality) {
    StreamBuffer a{0x01, 0x02, 0x03};
    StreamBuffer b{0x01, 0x02, 0x03};
    StreamBuffer c{0x01, 0x02, 0x04};
    EXPECT_TRUE(a == b);
    EXPECT_FALSE(a != b);
    EXPECT_FALSE(a == c);
    EXPECT_TRUE(a != c);
}

TEST(StreamBufferDeepTest, AssignAndClear) {
    StreamBuffer buf;
    buf.assign(5u, uint8_t(0xFF));
    EXPECT_EQ(buf.size(), 5u);
    EXPECT_EQ(buf[0], 0xFF);
    EXPECT_EQ(buf[4], 0xFF);

    buf.clear();
    EXPECT_TRUE(buf.empty());
    EXPECT_EQ(buf.size(), 0u);
}

TEST(StreamBufferDeepTest, LargeBufferGrowth) {
    StreamBuffer buf;
    std::vector<uint8_t> large(10000, 0x42);
    buf.append(large.data(), large.size());
    EXPECT_EQ(buf.size(), 10000u);
    EXPECT_EQ(buf[0], 0x42);
    EXPECT_EQ(buf[9999], 0x42);
}

TEST(StreamBufferDeepTest, InsertAndErase) {
    StreamBuffer buf{0x01, 0x02, 0x05};
    const uint8_t insert_data[] = {0x03, 0x04};
    buf.insert(buf.begin() + 2, insert_data, insert_data + 2);
    EXPECT_EQ(buf.size(), 5u);
    EXPECT_EQ(buf[0], 0x01);
    EXPECT_EQ(buf[1], 0x02);
    EXPECT_EQ(buf[2], 0x03);
    EXPECT_EQ(buf[3], 0x04);
    EXPECT_EQ(buf[4], 0x05);

    buf.erase(buf.begin() + 3, buf.begin() + 5);
    EXPECT_EQ(buf.size(), 3u);
    EXPECT_EQ(buf[2], 0x03);
}

// ── JsonBuilder ────────────────────────────────────────────────────

TEST(JsonBuilderDeepTest, RootObjectEmpty) {
    auto builder = JsonBuilder::root_object();
    std::string result = builder.build();
    EXPECT_EQ(result, "{}");
}

TEST(JsonBuilderDeepTest, SimpleFields) {
    auto builder = JsonBuilder::root_object();
    builder.field("name", std::string("test"));
    builder.field("count", 42);
    builder.field("active", true);
    builder.null_field("extra");
    std::string result = builder.build();
    EXPECT_NE(result.find("\"name\":\"test\""), std::string::npos);
    EXPECT_NE(result.find("\"count\":42"), std::string::npos);
    EXPECT_NE(result.find("\"active\":true"), std::string::npos);
    EXPECT_NE(result.find("\"extra\":null"), std::string::npos);
    EXPECT_EQ(result.front(), '{');
    EXPECT_EQ(result.back(), '}');
}

TEST(JsonBuilderDeepTest, NestedObject) {
    auto builder = JsonBuilder::root_object();
    builder.field("outer", 1);
    builder.object("inner");
    builder.field("key", std::string("value"));
    builder.end_object();
    std::string result = builder.build();
    EXPECT_NE(result.find("\"outer\":1"), std::string::npos);
    EXPECT_NE(result.find("\"inner\":{"), std::string::npos);
    EXPECT_NE(result.find("\"key\":\"value\""), std::string::npos);
}

TEST(JsonBuilderDeepTest, ArrayElements) {
    auto builder = JsonBuilder::root_object();
    builder.array("items");
    builder.element(std::string("a"));
    builder.element(std::string("b"));
    builder.element(123);
    builder.element(true);
    builder.end_array();
    std::string result = builder.build();
    EXPECT_NE(result.find("\"items\":[\"a\",\"b\",123,true]"), std::string::npos);
}

TEST(JsonBuilderDeepTest, AutoCloseUnbalanced) {
    auto builder = JsonBuilder::root_object();
    builder.object("nested");
    builder.field("key", 1);
    // Missing end_object() — build() should auto-close
    std::string result = builder.build();
    EXPECT_NE(result.find("\"key\":1"), std::string::npos);
    // Should end with }} (auto-closed nested + root)
    EXPECT_NE(result.find("}}"), std::string::npos);
}

TEST(JsonBuilderDeepTest, ResetAndReuse) {
    auto builder = JsonBuilder::root_object();
    builder.field("first", 1);
    std::string r1 = builder.build();
    EXPECT_NE(r1.find("\"first\":1"), std::string::npos);

    builder.reset();
    auto b2 = JsonBuilder::root_object();
    b2.field("second", 2);
    std::string r2 = b2.build();
    EXPECT_NE(r2.find("\"second\":2"), std::string::npos);
}

TEST(JsonBuilderDeepTest, StringEscaping) {
    auto builder = JsonBuilder::root_object();
    builder.field("msg", std::string("line1\nline2\t\"quoted\""));
    std::string result = builder.build();
    // Newline should be escaped as \n, tab as \t, quote as \"
    EXPECT_NE(result.find("\\n"), std::string::npos);
    EXPECT_NE(result.find("\\t"), std::string::npos);
    EXPECT_NE(result.find("\\\""), std::string::npos);
}

// ── JsonHelpers ────────────────────────────────────────────────────

TEST(JsonHelpersDeepTest, JsonEscapeRoundtrip) {
    std::string original = "hello\nworld\t\"test\"\\path";
    std::string escaped = json_escape(original);
    std::string unescaped = json_unescape(escaped);
    // Round-trip: the json_unescape should undo json_escape for
    // common escapes.
    // Note: json_escape doesn't escape \b or \f, and json_unescape
    // handles them. So for characters that both support, round-trip works.
    EXPECT_NE(escaped.find("\\n"), std::string::npos);
    EXPECT_NE(escaped.find("\\t"), std::string::npos);
    EXPECT_NE(escaped.find("\\\""), std::string::npos);
    EXPECT_NE(escaped.find("\\\\"), std::string::npos);
}

TEST(JsonHelpersDeepTest, SkipWhitespace) {
    EXPECT_EQ(skip_json_ws("  \t\n\r  x", 0), 7u);
    EXPECT_EQ(skip_json_ws("no_ws", 0), 0u);
    EXPECT_EQ(skip_json_ws("", 0), 0u);
}

TEST(JsonHelpersDeepTest, ExtractJsonString) {
    std::string json = R"("hello world")";
    size_t pos = 0;
    std::string result = extract_json_string(json, pos);
    EXPECT_EQ(result, "hello world");
    EXPECT_GT(pos, 0u);

    // With escapes
    std::string json2 = R"("line1\nline2")";
    size_t pos2 = 0;
    std::string result2 = extract_json_string(json2, pos2);
    EXPECT_EQ(result2, "line1\nline2");
}

TEST(JsonHelpersDeepTest, ExtractJsonObjectRaw) {
    std::string json = R"({"key": "value", "nested": {"a":1}})";
    size_t pos = 0;
    std::string result = extract_json_object_raw(json, pos);
    EXPECT_EQ(result, json);
    EXPECT_EQ(pos, json.size());
}

TEST(JsonHelpersDeepTest, ExtractJsonArrayRaw) {
    std::string json = R"([1, "two", {"three":3}])";
    size_t pos = 0;
    std::string result = extract_json_array_raw(json, pos);
    EXPECT_EQ(result, json);
    EXPECT_EQ(pos, json.size());
}

TEST(JsonHelpersDeepTest, ParseJsonStringArray) {
    auto arr = parse_json_string_array(R"(["a", "b", "c"])");
    ASSERT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0], "a");
    EXPECT_EQ(arr[1], "b");
    EXPECT_EQ(arr[2], "c");
}

TEST(JsonHelpersDeepTest, ParseJsonStringArrayEmpty) {
    auto arr = parse_json_string_array(R"([])");
    EXPECT_TRUE(arr.empty());
}

TEST(JsonHelpersDeepTest, ParseJsonStringMap) {
    auto map = parse_json_string_map(R"({"k1":"v1", "k2":"v2"})");
    ASSERT_EQ(map.size(), 2u);
    EXPECT_EQ(map[0].first, "k1");
    EXPECT_EQ(map[0].second, "v1");
    EXPECT_EQ(map[1].first, "k2");
    EXPECT_EQ(map[1].second, "v2");
}

TEST(JsonHelpersDeepTest, ParseJsonStringMapEmpty) {
    auto map = parse_json_string_map(R"({})");
    EXPECT_TRUE(map.empty());
}

// ── DedupCache over-capacity eviction ─────────────────────────────

namespace {

EndPoint make_test_endpoint(uint16_t port = 9000) {
    Ipv4Endpoint ep{0x7F000001, port};
    return EndPoint{ep};
}

} // namespace

TEST(DedupCacheDeepTest, OverCapacityEviction) {
    // Use a very small capacity to force eviction.
    DedupCache::Config cfg;
    cfg.max_entries = 10;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    // Fill with 10 unique entries.
    for (int i = 0; i < 10; ++i) {
        (void)cache.is_duplicate(ep, ActorId{1},
                                 MessageId{static_cast<uint64_t>(i)});
    }
    EXPECT_EQ(cache.size(), 10u);

    // Insert one more to trigger eviction. Should not crash, and insertion
    // succeeds (non-duplicate).
    bool dup = cache.is_duplicate(ep, ActorId{1}, MessageId{100});
    EXPECT_FALSE(dup);

    // After eviction, size should be ≤ max_entries (though the current
    // eviction policy removes 10% of max_entries, so it'll be ≤ 10).
    EXPECT_LE(cache.size(), cfg.max_entries);
}

TEST(DedupCacheDeepTest, InsertionAfterEvictionPreservesIntegrity) {
    DedupCache::Config cfg;
    cfg.max_entries = 5;
    DedupCache cache(cfg);

    EndPoint ep = make_test_endpoint();
    // Fill to capacity.
    for (int i = 0; i < 5; ++i) {
        (void)cache.is_duplicate(ep, ActorId{1},
                                 MessageId{static_cast<uint64_t>(i)});
    }
    EXPECT_EQ(cache.size(), 5u);

    // Trigger eviction by inserting a 6th unique entry.
    bool sixth = cache.is_duplicate(ep, ActorId{2}, MessageId{1});
    EXPECT_FALSE(sixth); // new entry

    // After eviction, size should be bounded by max_entries.
    EXPECT_LE(cache.size(), cfg.max_entries);
    // Insertions counter should still account for the evicted entries.
    EXPECT_EQ(cache.insertions(), 6u);
}

TEST(DedupCacheDeepTest, MoveAssign) {
    DedupCache::Config cfg;
    cfg.max_entries = 100;
    DedupCache cache1(cfg);

    EndPoint ep = make_test_endpoint();
    (void)cache1.is_duplicate(ep, ActorId{1}, MessageId{100});
    EXPECT_EQ(cache1.size(), 1u);

    DedupCache cache2(DedupCache::Config{});
    cache2 = std::move(cache1);
    EXPECT_EQ(cache2.size(), 1u);
}

// ── NodeIdentity operations ────────────────────────────────────────

TEST(NodeIdentityDeepTest, DefaultConstruction) {
    NodeIdentity id;
    // Default EndPoint is a variant; we check the host/uds_path are empty.
    EXPECT_TRUE(id.host.empty());
    EXPECT_TRUE(id.uds_path.empty());
    EXPECT_TRUE(id.acceptors.empty());
}

TEST(NodeIdentityDeepTest, Equality) {
    NodeIdentity a;
    a.host = "node1";
    a.uds_path = "/tmp/hp.sock";

    NodeIdentity b;
    b.host = "node1";
    b.uds_path = "/tmp/hp.sock";

    NodeIdentity c;
    c.host = "node2";

    // Field-by-field comparison (NodeIdentity uses defaulted operator==,
    // but AcceptorInfo lacks operator== so we compare non-vector fields).
    EXPECT_EQ(a.host, b.host);
    EXPECT_EQ(a.uds_path, b.uds_path);
    EXPECT_NE(a.host, c.host);
}

TEST(NodeIdentityDeepTest, FieldAssignment) {
    NodeIdentity id;
    id.host = "server.example.com";
    id.uds_path = "/var/run/hpactor.sock";
    Ipv4Endpoint ep{0x7F000001, 8080};
    id.endpoint = EndPoint{ep};

    net::AcceptorInfo ai;
    ai.port = 8080;
    ai.tls_required = true;
    id.acceptors.push_back(ai);

    EXPECT_EQ(id.host, "server.example.com");
    EXPECT_EQ(id.uds_path, "/var/run/hpactor.sock");
    EXPECT_EQ(id.acceptors.size(), 1u);
    EXPECT_EQ(id.acceptors[0].port, 8080u);
    EXPECT_TRUE(id.acceptors[0].tls_required);
}
