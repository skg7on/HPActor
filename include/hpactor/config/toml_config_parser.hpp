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

#include <hpactor/config/toml_file_data.hpp>
#include <hpactor/config/toml_parse_context.hpp>
#include <hpactor/config/toml_table_view.hpp>
#include <hpactor/types/types.hpp>

#include <string_view>

namespace hpactor::config {

/// \brief Interface for parsing a [system] TOML subsection.
///
/// Subsystem parsers implement this interface and self-register via
/// TomlSystemParserRegistration<T>. Each parser is responsible for
/// reading one subsection of the [system] table.
///
/// \note Registration-time construction; parse() invoked during
///       TomlParser::parse().
class ITomlSystemConfigParser {
  public:
    virtual ~ITomlSystemConfigParser() = default;

    /// \brief Unique parser name for registration and diagnostics.
    ///
    /// \return A string_view to the parser name (typically a static literal).
    virtual std::string_view name() const noexcept = 0;

    /// \brief Registration ordering (lower values parse first).
    ///
    /// \return A non-negative integer priority.
    virtual int order() const noexcept = 0;

    /// \brief Parse the [system] table and populate \p out.
    ///
    /// \param[in] system The [system] TOML table view.
    /// \param[out] out The SystemDef to populate.
    /// \param[in,out] ctx Parse context for error reporting.
    /// \return success or an error result.
    virtual result<void> parse(const TomlTableView& system, SystemDef& out,
                               TomlParseContext& ctx) const = 0;
};

/// \brief Interface for parsing a top-level TOML document section.
///
/// Document-level parsers handle top-level TOML tables (e.g. dispatcher
/// definitions, template sections) beyond the [system] table. They
/// self-register via TomlDocumentParserRegistration<T>.
///
/// \note Registration-time construction; parse() invoked during
///       TomlParser::parse().
class ITomlDocumentConfigParser {
  public:
    virtual ~ITomlDocumentConfigParser() = default;

    /// \brief Unique parser name for registration and diagnostics.
    ///
    /// \return A string_view to the parser name.
    virtual std::string_view name() const noexcept = 0;

    /// \brief Registration ordering (lower values parse first).
    ///
    /// \return A non-negative integer priority.
    virtual int order() const noexcept = 0;

    /// \brief Parse a top-level section and populate \p out.
    ///
    /// \param[in] root The root TOML table view.
    /// \param[out] out The TomlFileData to populate.
    /// \param[in,out] ctx Parse context for error reporting.
    /// \return success or an error result.
    virtual result<void> parse(const TomlTableView& root, TomlFileData& out,
                               TomlParseContext& ctx) const = 0;
};

} // namespace hpactor::config
