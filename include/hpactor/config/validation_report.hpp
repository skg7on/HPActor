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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace hpactor::config {

/// \brief Severity level of a config validation finding.
///
/// Mirrors the design from
/// docs/architecture/production/dynamic-config-parser-ioc-design.md §6.
enum class ConfigSeverity : uint8_t {
    /// \brief Informational finding — does not affect correctness.
    Info,
    /// \brief Warning — non-fatal but should be reviewed.
    Warning,
    /// \brief Error — the config is invalid and should block startup.
    Error,
};

/// \brief A single validation finding with severity, dot-notation path,
///        and human-readable message.
///
/// Paths follow TOML key-path conventions, e.g.
/// "system.mailbox.low_watermark".  Errors and warnings that a subsystem
/// parser cannot report through its normal error return can be added to
/// a ValidationReport via TomlParseContext.
struct ConfigFinding {
    /// \brief Severity level.
    ConfigSeverity severity{ConfigSeverity::Info};
    /// \brief Dot-notation path to the config field (e.g.
    ///        "system.mailbox.low_watermark").
    std::string path;
    /// \brief Human-readable description of the issue.
    std::string message;
};

/// \brief Accumulates validation findings during config parsing.
///
/// Subsystem parsers receive a pointer to the report through
/// TomlParseContext and call add_error / add_warning / add_info for
/// non-fatal issues.  Fatal parse errors are still communicated through
/// the existing result&lt;void&gt; return channel.
class ValidationReport {
  public:
    /// \brief Add a finding at Error severity.
    ///
    /// \param[in] path Dot-notation config field path.
    /// \param[in] message Human-readable description.
    void add_error(std::string path, std::string message) {
        ++error_count_;
        findings_.push_back(
            {ConfigSeverity::Error, std::move(path), std::move(message)});
    }

    /// \brief Add a finding at Warning severity.
    ///
    /// \param[in] path Dot-notation config field path.
    /// \param[in] message Human-readable description.
    void add_warning(std::string path, std::string message) {
        ++warning_count_;
        findings_.push_back(
            {ConfigSeverity::Warning, std::move(path), std::move(message)});
    }

    /// \brief Add a finding at Info severity.
    ///
    /// \param[in] path Dot-notation config field path.
    /// \param[in] message Human-readable description.
    void add_info(std::string path, std::string message) {
        ++info_count_;
        findings_.push_back(
            {ConfigSeverity::Info, std::move(path), std::move(message)});
    }

    /// \brief Add a pre-constructed ConfigFinding.
    ///
    /// \param[in] finding The finding to add.
    void add(ConfigFinding finding) {
        switch (finding.severity) {
            case ConfigSeverity::Error:
                ++error_count_;
                break;
            case ConfigSeverity::Warning:
                ++warning_count_;
                break;
            case ConfigSeverity::Info:
                ++info_count_;
                break;
        }
        findings_.push_back(std::move(finding));
    }

    /// \brief Whether any Error-severity findings were reported.
    ///
    /// \return true if at least one Error finding exists.
    bool has_errors() const noexcept {
        return error_count_ > 0;
    }

    /// \brief Whether any Warning-severity findings were reported.
    ///
    /// \return true if at least one Warning finding exists.
    bool has_warnings() const noexcept {
        return warning_count_ > 0;
    }

    /// \brief Number of Error-severity findings.
    ///
    /// \return Error count.
    size_t error_count() const noexcept {
        return error_count_;
    }

    /// \brief Number of Warning-severity findings.
    ///
    /// \return Warning count.
    size_t warning_count() const noexcept {
        return warning_count_;
    }

    /// \brief Number of Info-severity findings.
    ///
    /// \return Info count.
    size_t info_count() const noexcept {
        return info_count_;
    }

    /// \brief Total number of findings.
    ///
    /// \return Sum of all finding counts.
    size_t total_count() const noexcept {
        return findings_.size();
    }

    /// \brief Const reference to all findings in insertion order.
    ///
    /// \return Vector of ConfigFinding.
    const std::vector<ConfigFinding>& findings() const noexcept {
        return findings_;
    }

  private:
    std::vector<ConfigFinding> findings_;
    size_t error_count_{0};
    size_t warning_count_{0};
    size_t info_count_{0};
};

} // namespace hpactor::config
