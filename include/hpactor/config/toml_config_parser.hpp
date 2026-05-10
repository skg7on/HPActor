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

class ITomlSystemConfigParser {
  public:
    virtual ~ITomlSystemConfigParser() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual int order() const noexcept = 0;
    virtual result<void> parse(const TomlTableView& system, SystemDef& out,
                               TomlParseContext& ctx) const = 0;
};

class ITomlDocumentConfigParser {
  public:
    virtual ~ITomlDocumentConfigParser() = default;

    virtual std::string_view name() const noexcept = 0;
    virtual int order() const noexcept = 0;
    virtual result<void> parse(const TomlTableView& root, TomlFileData& out,
                               TomlParseContext& ctx) const = 0;
};

} // namespace hpactor::config
