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

#include <hpactor/config/toml_parse_context.hpp>
#include <hpactor/config/validation_report.hpp>

#include <gtest/gtest.h>

using namespace hpactor::config;

// ── ConfigSeverity ─────────────────────────────────────────────────

TEST(ConfigSeverityTest, HasThreeLevels) {
    // Verify the three severity levels are distinct.
    EXPECT_NE(static_cast<uint8_t>(ConfigSeverity::Info),
              static_cast<uint8_t>(ConfigSeverity::Warning));
    EXPECT_NE(static_cast<uint8_t>(ConfigSeverity::Warning),
              static_cast<uint8_t>(ConfigSeverity::Error));
    EXPECT_NE(static_cast<uint8_t>(ConfigSeverity::Info),
              static_cast<uint8_t>(ConfigSeverity::Error));
}

// ── ConfigFinding ──────────────────────────────────────────────────

TEST(ConfigFindingTest, DefaultConstruction) {
    ConfigFinding f;
    EXPECT_EQ(f.severity, ConfigSeverity::Info);
    EXPECT_TRUE(f.path.empty());
    EXPECT_TRUE(f.message.empty());
}

TEST(ConfigFindingTest, FullConstruction) {
    ConfigFinding f{ConfigSeverity::Error, "system.mailbox.low_watermark",
                    "value out of range"};
    EXPECT_EQ(f.severity, ConfigSeverity::Error);
    EXPECT_EQ(f.path, "system.mailbox.low_watermark");
    EXPECT_EQ(f.message, "value out of range");
}

// ── ValidationReport — empty ───────────────────────────────────────

TEST(ValidationReportTest, DefaultConstructed) {
    ValidationReport report;
    EXPECT_FALSE(report.has_errors());
    EXPECT_FALSE(report.has_warnings());
    EXPECT_EQ(report.error_count(), 0u);
    EXPECT_EQ(report.warning_count(), 0u);
    EXPECT_EQ(report.info_count(), 0u);
    EXPECT_EQ(report.total_count(), 0u);
    EXPECT_TRUE(report.findings().empty());
}

// ── ValidationReport — add via convenience methods ─────────────────

TEST(ValidationReportTest, AddError) {
    ValidationReport report;
    report.add_error("system.mailbox", "capacity must be > 0");
    EXPECT_TRUE(report.has_errors());
    EXPECT_FALSE(report.has_warnings());
    EXPECT_EQ(report.error_count(), 1u);
    EXPECT_EQ(report.warning_count(), 0u);
    EXPECT_EQ(report.info_count(), 0u);
    EXPECT_EQ(report.total_count(), 1u);

    const auto& f = report.findings()[0];
    EXPECT_EQ(f.severity, ConfigSeverity::Error);
    EXPECT_EQ(f.path, "system.mailbox");
    EXPECT_EQ(f.message, "capacity must be > 0");
}

TEST(ValidationReportTest, AddWarning) {
    ValidationReport report;
    report.add_warning("system.mailbox.low_watermark", "value -0.5 clamped to 0.50");
    EXPECT_FALSE(report.has_errors());
    EXPECT_TRUE(report.has_warnings());
    EXPECT_EQ(report.error_count(), 0u);
    EXPECT_EQ(report.warning_count(), 1u);
    EXPECT_EQ(report.total_count(), 1u);

    const auto& f = report.findings()[0];
    EXPECT_EQ(f.severity, ConfigSeverity::Warning);
}

TEST(ValidationReportTest, AddInfo) {
    ValidationReport report;
    report.add_info("system.version", "using schema version 1.0");
    EXPECT_FALSE(report.has_errors());
    EXPECT_FALSE(report.has_warnings());
    EXPECT_EQ(report.info_count(), 1u);
}

// ── ValidationReport — mixed findings ──────────────────────────────

TEST(ValidationReportTest, MultipleFindingsPreserveOrder) {
    ValidationReport report;
    report.add_error("a", "error 1");
    report.add_warning("b", "warning 1");
    report.add_info("c", "info 1");
    report.add_error("d", "error 2");

    EXPECT_EQ(report.total_count(), 4u);
    EXPECT_EQ(report.error_count(), 2u);
    EXPECT_EQ(report.warning_count(), 1u);
    EXPECT_EQ(report.info_count(), 1u);
    EXPECT_TRUE(report.has_errors());
    EXPECT_TRUE(report.has_warnings());

    // Verify insertion order is preserved.
    const auto& findings = report.findings();
    ASSERT_EQ(findings.size(), 4u);
    EXPECT_EQ(findings[0].severity, ConfigSeverity::Error);
    EXPECT_EQ(findings[0].path, "a");
    EXPECT_EQ(findings[1].severity, ConfigSeverity::Warning);
    EXPECT_EQ(findings[1].path, "b");
    EXPECT_EQ(findings[2].severity, ConfigSeverity::Info);
    EXPECT_EQ(findings[2].path, "c");
    EXPECT_EQ(findings[3].severity, ConfigSeverity::Error);
    EXPECT_EQ(findings[3].path, "d");
}

// ── ValidationReport — add(ConfigFinding) ──────────────────────────

TEST(ValidationReportTest, AddFindingStruct) {
    ValidationReport report;
    ConfigFinding f{ConfigSeverity::Warning, "system.transport",
                    "deprecated setting"};
    report.add(f);
    EXPECT_EQ(report.total_count(), 1u);
    EXPECT_EQ(report.warning_count(), 1u);
}

// ── TomlParseContext — validation report integration ───────────────

TEST(TomlParseContextTest, DefaultHasNoReport) {
    TomlParseContext ctx("test.toml", true);
    EXPECT_EQ(ctx.report(), nullptr);
}

TEST(TomlParseContextTest, SetAndUseReport) {
    ValidationReport report;
    TomlParseContext ctx("test.toml", true);
    ctx.set_report(&report);

    EXPECT_EQ(ctx.report(), &report);

    ctx.add_finding(ConfigSeverity::Error, "system.foo", "foo must be positive");
    ctx.add_finding(ConfigSeverity::Warning, "system.bar", "bar is deprecated");

    EXPECT_EQ(report.error_count(), 1u);
    EXPECT_EQ(report.warning_count(), 1u);
    EXPECT_EQ(report.total_count(), 2u);

    const auto& findings = report.findings();
    EXPECT_EQ(findings[0].severity, ConfigSeverity::Error);
    EXPECT_EQ(findings[0].path, "system.foo");
    EXPECT_EQ(findings[1].severity, ConfigSeverity::Warning);
    EXPECT_EQ(findings[1].path, "system.bar");
}

TEST(TomlParseContextTest, AddFindingNoReportIsSafe) {
    // When no report is set, add_finding should be a no-op (no crash).
    TomlParseContext ctx("test.toml", true);
    ctx.add_finding(ConfigSeverity::Error, "system.x", "should not crash");
    // If we get here without crashing, the test passes.
    SUCCEED();
}
