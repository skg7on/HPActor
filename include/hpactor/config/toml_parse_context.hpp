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

#include <hpactor/config/validation_report.hpp>
#include <hpactor/types/types.hpp>

#include <string>

namespace hpactor::config {

/// \brief Per-file parse context for error reporting and path tracking.
///
/// Carries the file path being parsed and whether it is the entrypoint
/// file (vs an imported file). Passed to subsystem parsers so they can
/// report file-specific errors.
///
/// Also carries an optional ValidationReport pointer.  Subsystem parsers
/// that detect non-fatal issues (type mismatches, value clamping,
/// deprecated keys) can call add_finding() to record them without
/// aborting the parse.
class TomlParseContext {
  public:
    /// \brief Construct a parse context.
    ///
    /// \param[in] filepath Path to the TOML file being parsed.
    /// \param[in] entrypoint Whether this is the root entrypoint file.
    TomlParseContext(std::string filepath, bool entrypoint) noexcept
        : filepath_(std::move(filepath)), entrypoint_(entrypoint) {}

    /// \brief The file path being parsed.
    ///
    /// \return Const reference to the file path string.
    const std::string& filepath() const noexcept {
        return filepath_;
    }

    /// \brief Whether this file is the entrypoint (vs an import).
    ///
    /// \return true if this is the root file passed to TomlParser::parse().
    bool is_entrypoint() const noexcept {
        return entrypoint_;
    }

    /// \brief Produce an error result tagged with this file's context.
    ///
    /// \param[in] message Human-readable error description.
    /// \return An error result.
    result<void> fail(const char* message) const {
        return result<void>::make(error(errors::invalid_argument,
                                        std::string("TOML parse error in ") +
                                            filepath_ + ": " + message));
    }

    /// \brief Set the validation report for this parse context.
    ///
    /// After setting, subsystem parsers can call add_finding() to record
    /// non-fatal issues.
    ///
    /// \param[in] report Non-owning pointer to the report.
    void set_report(ValidationReport* report) noexcept {
        report_ = report;
    }

    /// \brief The current validation report, or nullptr if not set.
    ///
    /// \return Non-owning pointer to the report.
    ValidationReport* report() const noexcept {
        return report_;
    }

    /// \brief Add a validation finding to the report.
    ///
    /// Safe to call when no report is set — the call is silently ignored.
    ///
    /// \param[in] severity Finding severity.
    /// \param[in] path Dot-notation config field path.
    /// \param[in] message Human-readable description.
    void add_finding(ConfigSeverity severity, std::string path,
                     std::string message) const {
        if (report_)
            report_->add({severity, std::move(path), std::move(message)});
    }

  private:
    std::string filepath_;
    bool entrypoint_{false};
    ValidationReport* report_{nullptr};
};

} // namespace hpactor::config
