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

#include <atomic>
#include <gtest/gtest.h>
#include <hpactor/log/log_category.hpp>
#include <hpactor/log/log_config.hpp>
#include <hpactor/log/log_event.hpp>
#include <hpactor/log/log_field.hpp>
#include <hpactor/log/log_formatter.hpp>
#include <hpactor/log/log_level.hpp>
#include <hpactor/log/log_manager.hpp>
#include <hpactor/log/log_ring_buffer.hpp>
#include <hpactor/log/log_sink.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/types/types.hpp>
#include <thread>
#include <vector>

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

using namespace hpactor;
using namespace hpactor::log;

namespace {

static LogEvent
make_event(LogLevel level = LogLevel::kInfo,
           LogCategory cat = LogCategory::kUser, const char* msg = "test") {
    LogEvent evt{};
    evt.level = level;
    evt.category = cat;
    evt.message = msg;
    return evt;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

static bool file_exists(const std::string& path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

} // namespace

// ---------------------------------------------------------------------------
// LogRingBuffer branch coverage
// ---------------------------------------------------------------------------

TEST(LogRingBufferBranchesTest, DrainWithTransformCallback) {
    log::LogRingBuffer rb(64);
    ASSERT_TRUE(rb.try_push(make_event()));

    int call_count = 0;
    size_t drained = rb.drain([&](const log::LogEvent& e) {
        EXPECT_STREQ(e.message, "test");
        call_count++;
    });
    EXPECT_EQ(drained, 1u);
    EXPECT_EQ(call_count, 1);
    EXPECT_TRUE(rb.empty());
}

TEST(LogRingBufferBranchesTest, DrainEmptyBuffer) {
    log::LogRingBuffer rb(64);
    ASSERT_TRUE(rb.empty());

    int call_count = 0;
    size_t drained = rb.drain([&](const log::LogEvent&) { call_count++; });
    EXPECT_EQ(drained, 0u);
    EXPECT_EQ(call_count, 0);
}

TEST(LogRingBufferBranchesTest, ConsecutiveDrainCycles) {
    log::LogRingBuffer rb(64);

    // Push 3, drain 3
    for (int i = 0; i < 3; i++) {
        rb.try_push(make_event(LogLevel::kInfo, LogCategory::kUser, "batch1"));
    }
    int count = 0;
    rb.drain([&](const log::LogEvent&) { count++; });
    EXPECT_EQ(count, 3);
    EXPECT_TRUE(rb.empty());

    // Push 2 more, drain 2
    for (int i = 0; i < 2; i++) {
        rb.try_push(make_event(LogLevel::kDebug, LogCategory::kActor, "batch2"));
    }
    count = 0;
    rb.drain([&](const log::LogEvent& e) {
        EXPECT_STREQ(e.message, "batch2");
        count++;
    });
    EXPECT_EQ(count, 2);
    EXPECT_TRUE(rb.empty());
}

TEST(LogRingBufferBranchesTest, PushAtExactCapacity) {
    log::LogRingBuffer rb(16);
    int pushed = 0;
    for (int i = 0; i < 16; i++) {
        if (rb.try_push(make_event()))
            pushed++;
    }
    EXPECT_EQ(pushed, 16);
    EXPECT_EQ(rb.size(), 16u);
    EXPECT_FALSE(rb.empty());
    EXPECT_EQ(rb.events_lost(), 0u);

    // One more should overflow
    EXPECT_FALSE(rb.try_push(make_event()));
    EXPECT_EQ(rb.events_lost(), 1u);
}

// ---------------------------------------------------------------------------
// LogDrain batching behaviour (via LogManager lifecycle)
// ---------------------------------------------------------------------------

TEST(LogManagerBranchesTest, StartStopLifecycle) {
    log::LogConfig config;
    config.ring_buffer_capacity = 64;
    config.default_level = LogLevel::kInfo;

    log::LogManager manager(config);

    // Verify initial state before start
    EXPECT_EQ(manager.events_lost(), 0u);
    EXPECT_EQ(manager.sink_errors(), 0u);

    // LogManager configures global_logger(), so check through it
    auto& logger = global_logger();
    EXPECT_TRUE(logger.enabled(LogLevel::kInfo, LogCategory::kUser));

    // Start the drain (spawns drain thread)
    manager.start();

    // Emit some log events through the global logger
    for (int i = 0; i < 10; i++) {
        logger.emit(make_event(LogLevel::kInfo, LogCategory::kUser, "drain_test"));
    }

    // Stop the drain — final drain happens in stop()
    manager.stop();

    // After stop, events_lost should reflect any drops
    // (ring buffer is 64, we pushed 10 — none should be lost)
    EXPECT_EQ(manager.events_lost(), 0u);
}

TEST(LogManagerBranchesTest, EventsLostWhenBufferOverflows) {
    log::LogConfig config;
    config.ring_buffer_capacity = 4; // very small buffer to force overflows
    config.default_level = LogLevel::kInfo;

    log::LogManager manager(config);

    // Push events directly to ring buffer (bypassing logger) to overflow
    // The drain was NOT started, so events will accumulate
    auto& rb = manager.ring_buffer();
    for (int i = 0; i < 20; i++) {
        rb.try_push(make_event(LogLevel::kInfo, LogCategory::kUser, "overflow"));
    }

    EXPECT_GT(manager.events_lost(), 0u);
}

TEST(LogManagerBranchesTest, SinkErrorsInitiallyZero) {
    log::LogConfig config;
    config.ring_buffer_capacity = 64;

    log::LogManager manager(config);

    // Before any drain activity, sink_errors should be zero
    EXPECT_EQ(manager.sink_errors(), 0u);
}

TEST(LogManagerBranchesTest, ConfigAccessorReflectsInput) {
    log::LogConfig config;
    config.default_level = LogLevel::kDebug;
    config.format = log::LogFormat::kText;
    config.ring_buffer_capacity = 1024;

    log::LogManager manager(config);

    EXPECT_EQ(manager.config().default_level, LogLevel::kDebug);
    EXPECT_EQ(manager.config().format, log::LogFormat::kText);
    EXPECT_EQ(manager.config().ring_buffer_capacity, 1024u);
}

// ---------------------------------------------------------------------------
// LogSink memory variant branch coverage
// ---------------------------------------------------------------------------

TEST(LogMemorySinkBranchesTest, WriteMultipleAndRetrieve) {
    auto sink = std::make_unique<log::MemorySink>();

    EXPECT_TRUE(sink->write("line1").has_value());
    EXPECT_TRUE(sink->write("line2").has_value());
    EXPECT_TRUE(sink->write("line3").has_value());

    auto all_lines = sink->lines();
    ASSERT_EQ(all_lines.size(), 3u);
    EXPECT_EQ(all_lines[0], "line1");
    EXPECT_EQ(all_lines[1], "line2");
    EXPECT_EQ(all_lines[2], "line3");
}

TEST(LogMemorySinkBranchesTest, ClearEmptiesLines) {
    auto sink = std::make_unique<log::MemorySink>();
    sink->write("hello");
    sink->write("world");
    ASSERT_EQ(sink->lines().size(), 2u);

    sink->clear();
    EXPECT_TRUE(sink->lines().empty());
}

TEST(LogMemorySinkBranchesTest, FlushReturnsSuccess) {
    auto sink = std::make_unique<log::MemorySink>();
    EXPECT_TRUE(sink->flush().has_value());
}

// ---------------------------------------------------------------------------
// LogFormatter branch coverage
// ---------------------------------------------------------------------------

TEST(LogFormatterBranchesTest, TextFormatIncludesAllFieldTypes) {
    log::TextLogFormatter fmt;

    log::LogEvent evt{};
    evt.timestamp_ns = 1000000000;
    evt.level = LogLevel::kError;
    evt.category = LogCategory::kMemory;
    evt.actor_id = ActorId{3};
    evt.line = 42;
    evt.file = "test.cpp";
    evt.message = "Memory test";
    evt.field_count = 0;

    std::string out;
    fmt.format(evt, out);

    EXPECT_NE(out.find("error"), std::string::npos);
    EXPECT_NE(out.find("memory"), std::string::npos);
    EXPECT_NE(out.find("Memory test"), std::string::npos);
}

TEST(LogFormatterBranchesTest, JsonFormatHandlesEmptyMessage) {
    log::JsonLogFormatter fmt;

    log::LogEvent evt{};
    evt.timestamp_ns = 2000000000;
    evt.level = LogLevel::kWarning;
    evt.category = LogCategory::kConfig;
    evt.message = "";

    std::string out;
    fmt.format(evt, out);

    EXPECT_NE(out.find("\"warning\""), std::string::npos);
    EXPECT_NE(out.find("\"config\""), std::string::npos);
}

TEST(LogFormatterBranchesTest, JsonFormatIncludesActorId) {
    log::JsonLogFormatter fmt;

    log::LogEvent evt{};
    evt.timestamp_ns = 3000000000;
    evt.level = LogLevel::kInfo;
    evt.category = LogCategory::kActor;
    evt.actor_id = ActorId{42};

    std::string out;
    fmt.format(evt, out);

    EXPECT_NE(out.find("\"actor_id\":42"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Logger enabled() / level filtering branch coverage
// ---------------------------------------------------------------------------

class LoggerBranchesTest : public ::testing::Test {
  protected:
    void SetUp() override {
        levels_ = {
            LogLevel::kInfo, // kActor
            LogLevel::kInfo, // kActorState
            LogLevel::kInfo, // kMailbox
            LogLevel::kInfo, // kScheduler
            LogLevel::kInfo, // kMemory
            LogLevel::kInfo, // kRegistrar
            LogLevel::kInfo, // kDiscovery
            LogLevel::kInfo, // kNetwork
            LogLevel::kInfo, // kRpc
            LogLevel::kInfo, // kConfig
            LogLevel::kInfo, // kSupervision
            LogLevel::kInfo, // kCli
            LogLevel::kInfo, // kHttp
            LogLevel::kInfo, // kFault
            LogLevel::kInfo, // kUser
        };
        log::global_logger().configure(&rb_, &levels_, LogLevel::kError);
    }

    void TearDown() override {
        log::global_logger().configure(nullptr, nullptr, LogLevel::kCritical);
    }

    log::LogRingBuffer rb_{64};
    std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)> levels_;
};

TEST_F(LoggerBranchesTest, CriticalAlwaysEnabled) {
    auto& logger = log::global_logger();
    // Critical (value 0) should always be enabled when threshold >= critical
    EXPECT_TRUE(logger.enabled(LogLevel::kCritical, LogCategory::kUser));
}

TEST_F(LoggerBranchesTest, InfoEnabledForInfoThreshold) {
    auto& logger = log::global_logger();
    EXPECT_TRUE(logger.enabled(LogLevel::kInfo, LogCategory::kUser));
}

TEST_F(LoggerBranchesTest, DebugDisabledForInfoThreshold) {
    auto& logger = log::global_logger();
    // Debug (value 4) > Info (value 3) — should be disabled
    EXPECT_FALSE(logger.enabled(LogLevel::kDebug, LogCategory::kUser));
}

TEST_F(LoggerBranchesTest, TraceDisabledForInfoThreshold) {
    auto& logger = log::global_logger();
    EXPECT_FALSE(logger.enabled(LogLevel::kTrace, LogCategory::kUser));
}

TEST_F(LoggerBranchesTest, ErrorEnabledForInfoThreshold) {
    auto& logger = log::global_logger();
    // Error (value 1) <= Info (value 3) — should be enabled
    EXPECT_TRUE(logger.enabled(LogLevel::kError, LogCategory::kUser));
}

TEST_F(LoggerBranchesTest, WarningEnabledForInfoThreshold) {
    auto& logger = log::global_logger();
    EXPECT_TRUE(logger.enabled(LogLevel::kWarning, LogCategory::kUser));
}

TEST_F(LoggerBranchesTest, CategoryWithOffThresholdDisablesAll) {
    // Set kConfig category to kOff
    levels_[static_cast<size_t>(LogCategory::kConfig)] = LogLevel::kOff;
    log::global_logger().configure(&rb_, &levels_, LogLevel::kError);

    auto& logger = log::global_logger();
    EXPECT_FALSE(logger.enabled(LogLevel::kCritical, LogCategory::kConfig));
    EXPECT_FALSE(logger.enabled(LogLevel::kError, LogCategory::kConfig));
    EXPECT_FALSE(logger.enabled(LogLevel::kInfo, LogCategory::kConfig));
}

TEST_F(LoggerBranchesTest, PerCategoryLevelOverride) {
    // Set kRpc category to kError (more restrictive)
    levels_[static_cast<size_t>(LogCategory::kRpc)] = LogLevel::kError;
    log::global_logger().configure(&rb_, &levels_, LogLevel::kError);

    auto& logger = log::global_logger();

    // Critical should still be enabled for kRpc (0 <= 1)
    EXPECT_TRUE(logger.enabled(LogLevel::kCritical, LogCategory::kRpc));
    // Error should be enabled for kRpc (1 <= 1)
    EXPECT_TRUE(logger.enabled(LogLevel::kError, LogCategory::kRpc));
    // Warning should be disabled for kRpc (2 > 1)
    EXPECT_FALSE(logger.enabled(LogLevel::kWarning, LogCategory::kRpc));
    // kUser should still be at Info level
    EXPECT_TRUE(logger.enabled(LogLevel::kWarning, LogCategory::kUser));
}

TEST_F(LoggerBranchesTest, EmitIncrementsFieldsDroppedOnOverflow) {
    auto& logger = log::global_logger();

    // Emit with more fields than kMaxLogFields (4)
    LogField fields[] = {
        log::field("a", int64_t{1}), log::field("b", int64_t{2}),
        log::field("c", int64_t{3}), log::field("d", int64_t{4}),
        log::field("e", int64_t{5}), // this one should be dropped
    };
    constexpr uint8_t extra_fields = 5;

    logger.emit(LogLevel::kError, LogCategory::kUser, ActorId{1},
                static_cast<uint32_t>(LogEventId::kActorSpawned), "fields_test",
                fields, extra_fields, __FILE__, __LINE__);

    EXPECT_EQ(logger.fields_dropped(), 1u);
}

// ---------------------------------------------------------------------------
// RotatingFileSink multiple rotation cycles
// ---------------------------------------------------------------------------

TEST(RotatingFileSinkBranchesTest, MultipleRotationCycles) {
    const char* base_path = "/tmp/hpactor_test_multi_rotate.log";
    // Clean up any previous test artifacts
    std::remove(base_path);
    std::remove((std::string(base_path) + ".1").c_str());
    std::remove((std::string(base_path) + ".2").c_str());

    log::RotatingFileConfig cfg;
    cfg.path = base_path;
    cfg.max_bytes = 50; // Small threshold to trigger multiple rotations
    cfg.max_files = 2;

    auto sink = log::make_rotating_file_sink(cfg);
    ASSERT_NE(sink, nullptr);

    // Write first chunk — fills to ~30 bytes, no rotation yet
    EXPECT_TRUE(sink->write("chunk_one____AAAAABBBBBCCCCCDDDDD").has_value());
    EXPECT_TRUE(sink->flush().has_value());

    // Write second chunk — should trigger rotation: current → .1
    EXPECT_TRUE(sink->write("chunk_two____EEEEEFFFFFGGGGGHHHHH").has_value());
    EXPECT_TRUE(sink->flush().has_value());

    // Write third chunk — should trigger another rotation:
    // .1 → .2, current → .1, open fresh
    EXPECT_TRUE(sink->write("chunk_three__IIIIIJJJJJKKKKKLLLLL").has_value());
    EXPECT_TRUE(sink->flush().has_value());

    std::string rotated1 = std::string(base_path) + ".1";
    std::string rotated2 = std::string(base_path) + ".2";

    EXPECT_TRUE(file_exists(rotated1));

    std::string current_content = read_file(base_path);
    EXPECT_NE(current_content.find("chunk_three"), std::string::npos);

    // Cleanup
    std::remove(base_path);
    std::remove(rotated1.c_str());
    std::remove(rotated2.c_str());
}

// ---------------------------------------------------------------------------
// Log category filtering via LogManager defaults
// ---------------------------------------------------------------------------

TEST(LogCategoryFilteringBranchesTest, NoisyCategoriesDefaultToWarning) {
    log::LogConfig config;
    config.default_level = LogLevel::kDebug; // permissive default
    config.ring_buffer_capacity = 64;

    log::LogManager manager(config);

    // LogManager configures global_logger() with resolved levels
    auto& logger = global_logger();

    // Noisy categories should be at Warning: Debug and Trace disabled
    EXPECT_FALSE(logger.enabled(LogLevel::kDebug, LogCategory::kMailbox));
    EXPECT_FALSE(logger.enabled(LogLevel::kTrace, LogCategory::kMemory));
    EXPECT_FALSE(logger.enabled(LogLevel::kDebug, LogCategory::kNetwork));
    EXPECT_FALSE(logger.enabled(LogLevel::kDebug, LogCategory::kActorState));
    EXPECT_FALSE(logger.enabled(LogLevel::kDebug, LogCategory::kScheduler));

    // Warning should still be enabled for noisy categories
    EXPECT_TRUE(logger.enabled(LogLevel::kWarning, LogCategory::kMailbox));
    EXPECT_TRUE(logger.enabled(LogLevel::kWarning, LogCategory::kScheduler));

    // Non-noisy categories should use the configured default (kDebug)
    EXPECT_TRUE(logger.enabled(LogLevel::kDebug, LogCategory::kUser));
    EXPECT_TRUE(logger.enabled(LogLevel::kInfo, LogCategory::kActor));
}

TEST(LogCategoryFilteringBranchesTest, ExplicitOverrideWinsOverNoisyDefault) {
    log::LogConfig config;
    config.default_level = LogLevel::kInfo;
    config.ring_buffer_capacity = 64;

    // Override kMailbox (noisy) to kDebug
    config.levels[static_cast<size_t>(LogCategory::kMailbox)] = LogLevel::kDebug;

    log::LogManager manager(config);

    // LogManager configures global_logger() with resolved levels
    auto& logger = global_logger();

    // Mailbox should now allow debug
    EXPECT_TRUE(logger.enabled(LogLevel::kDebug, LogCategory::kMailbox));

    // Network (still noisy, no override) should be at kWarning
    EXPECT_FALSE(logger.enabled(LogLevel::kDebug, LogCategory::kNetwork));
    EXPECT_TRUE(logger.enabled(LogLevel::kWarning, LogCategory::kNetwork));
}

// ---------------------------------------------------------------------------
// Logger emit path (no fields, various levels)
// ---------------------------------------------------------------------------

TEST(LoggerEmitBranchesTest, EmitWithNoFields) {
    log::LogRingBuffer rb(64);
    std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)> levels{};
    levels.fill(LogLevel::kDebug);
    log::global_logger().configure(&rb, &levels, LogLevel::kError);

    auto& logger = log::global_logger();
    logger.emit(LogLevel::kInfo, LogCategory::kUser, ActorId{7},
                static_cast<uint32_t>(LogEventId::kActorSpawned),
                "plain message", nullptr, 0, __FILE__, __LINE__);

    EXPECT_FALSE(rb.empty());
    EXPECT_EQ(rb.size(), 1u);

    log::global_logger().configure(nullptr, nullptr, LogLevel::kCritical);
}

TEST(LoggerEmitBranchesTest, EmitThroughGlobalLoggerAfterConfigure) {
    log::LogRingBuffer rb(64);
    std::array<LogLevel, static_cast<size_t>(LogCategory::kCount)> levels{};
    levels.fill(LogLevel::kTrace); // allow everything
    log::global_logger().configure(&rb, &levels, LogLevel::kCritical);

    auto& logger = log::global_logger();
    EXPECT_TRUE(logger.enabled(LogLevel::kTrace, LogCategory::kUser));
    EXPECT_TRUE(logger.enabled(LogLevel::kDebug, LogCategory::kUser));
    EXPECT_TRUE(logger.enabled(LogLevel::kInfo, LogCategory::kActor));

    log::global_logger().configure(nullptr, nullptr, LogLevel::kCritical);
    EXPECT_FALSE(logger.enabled(LogLevel::kInfo, LogCategory::kUser));
}

// ---------------------------------------------------------------------------
// LogField factory function coverage
// ---------------------------------------------------------------------------

TEST(LogFieldBranchesTest, AllFieldFactoryFunctions) {
    auto i64_field = log::field("count", int64_t{-42});
    EXPECT_EQ(i64_field.type, log::LogFieldType::kInt64);
    EXPECT_STREQ(i64_field.name, "count");
    EXPECT_EQ(i64_field.value.i64, -42);

    auto u64_field = log::field("id", uint64_t{12345});
    EXPECT_EQ(u64_field.type, log::LogFieldType::kUInt64);
    EXPECT_STREQ(u64_field.name, "id");
    EXPECT_EQ(u64_field.value.u64, 12345u);

    auto dbl_field = log::field("ratio", 3.14);
    EXPECT_EQ(dbl_field.type, log::LogFieldType::kDouble);
    EXPECT_STREQ(dbl_field.name, "ratio");
    EXPECT_DOUBLE_EQ(dbl_field.value.f64, 3.14);

    auto bool_field = log::field("active", true);
    EXPECT_EQ(bool_field.type, log::LogFieldType::kBool);
    EXPECT_STREQ(bool_field.name, "active");
    EXPECT_TRUE(bool_field.value.boolean);

    auto lit_field = log::field_lit("key", "value_literal");
    EXPECT_EQ(lit_field.type, log::LogFieldType::kStringLiteral);
    EXPECT_STREQ(lit_field.name, "key");
    EXPECT_STREQ(lit_field.value.str, "value_literal");

    int dummy_val = 7;
    auto ptr_field = log::field_ptr("addr", &dummy_val);
    EXPECT_EQ(ptr_field.type, log::LogFieldType::kPointer);
    EXPECT_STREQ(ptr_field.name, "addr");
    EXPECT_EQ(ptr_field.value.ptr, &dummy_val);
}
