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

#include <hpactor/config/toml_config_parser.hpp>

#include <memory>
#include <string>
#include <vector>

namespace hpactor::config {

class TomlParserRegistry {
  public:
    using SystemParserFactory = std::unique_ptr<ITomlSystemConfigParser> (*)();
    using DocumentParserFactory = std::unique_ptr<ITomlDocumentConfigParser> (*)();

    static TomlParserRegistry& instance();

    bool add_system_parser(std::string_view name, int order,
                           SystemParserFactory factory);
    bool add_document_parser(std::string_view name, int order,
                             DocumentParserFactory factory);

    std::vector<std::unique_ptr<ITomlSystemConfigParser>>
    create_system_parsers() const;

    std::vector<std::unique_ptr<ITomlDocumentConfigParser>>
    create_document_parsers() const;

  private:
    TomlParserRegistry() = default;

    struct SystemEntry {
        std::string name;
        int order{0};
        SystemParserFactory factory{nullptr};
    };

    struct DocumentEntry {
        std::string name;
        int order{0};
        DocumentParserFactory factory{nullptr};
    };

    mutable std::vector<SystemEntry> system_parsers_;
    mutable std::vector<DocumentEntry> document_parsers_;
};

template <typename ParserT> class TomlSystemParserRegistration {
  public:
    TomlSystemParserRegistration() {
        registered_ = TomlParserRegistry::instance().add_system_parser(
            ParserT::kName, ParserT::kOrder,
            []() -> std::unique_ptr<ITomlSystemConfigParser> {
                return std::make_unique<ParserT>();
            });
    }

    bool registered() const noexcept {
        return registered_;
    }

  private:
    bool registered_{false};
};

template <typename ParserT> class TomlDocumentParserRegistration {
  public:
    TomlDocumentParserRegistration() {
        registered_ = TomlParserRegistry::instance().add_document_parser(
            ParserT::kName, ParserT::kOrder,
            []() -> std::unique_ptr<ITomlDocumentConfigParser> {
                return std::make_unique<ParserT>();
            });
    }

    bool registered() const noexcept {
        return registered_;
    }

  private:
    bool registered_{false};
};

} // namespace hpactor::config
