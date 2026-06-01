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

/// \brief Singleton IoC registry for TOML subsystem parsers.
///
/// System parsers (ITomlSystemConfigParser) and document parsers
/// (ITomlDocumentConfigParser) self-register before main() via file-scope
/// TomlSystemParserRegistration<T> / TomlDocumentParserRegistration<T>
/// objects. The registry produces ordered parser instances at parse time.
///
/// \note Registration phase: static initialization (single-threaded).
///       Factory phase: called during TomlParser::parse().
class TomlParserRegistry {
  public:
    /// \brief Factory function type for system-level parsers.
    using SystemParserFactory = std::unique_ptr<ITomlSystemConfigParser> (*)();
    /// \brief Factory function type for document-level parsers.
    using DocumentParserFactory = std::unique_ptr<ITomlDocumentConfigParser> (*)();

    /// \brief Access the singleton instance.
    ///
    /// \return Reference to the process-global registry.
    static TomlParserRegistry& instance();

    /// \brief Register a system parser factory.
    ///
    /// \param[in] name Unique parser name.
    /// \param[in] order Registration order (lower = earlier).
    /// \param[in] factory Factory function.
    /// \retval true Registered successfully.
    /// \retval false A parser with this name is already registered.
    bool add_system_parser(std::string_view name, int order,
                           SystemParserFactory factory);

    /// \brief Register a document parser factory.
    ///
    /// \param[in] name Unique parser name.
    /// \param[in] order Registration order (lower = earlier).
    /// \param[in] factory Factory function.
    /// \retval true Registered successfully.
    /// \retval false A parser with this name is already registered.
    bool add_document_parser(std::string_view name, int order,
                             DocumentParserFactory factory);

    /// \brief Create all registered system parsers in registration order.
    ///
    /// \return Vector of parser instances.
    std::vector<std::unique_ptr<ITomlSystemConfigParser>>
    create_system_parsers() const;

    /// \brief Create all registered document parsers in registration order.
    ///
    /// \return Vector of parser instances.
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

/// \brief Static file-scope registrar for ITomlSystemConfigParser subclasses.
///
/// Instantiate as a const file-scope object in a parser .cpp file:
/// \code{.cpp}
/// const TomlSystemParserRegistration<MySystemParser> kRegisterMyParser;
/// \endcode
///
/// \tparam ParserT Concrete ITomlSystemConfigParser subclass with static
///                 \c kName (string_view) and \c kOrder (int) members.
template <typename ParserT> class TomlSystemParserRegistration {
  public:
    /// \brief Registers the parser factory with the singleton registry.
    TomlSystemParserRegistration() {
        registered_ = TomlParserRegistry::instance().add_system_parser(
            ParserT::kName, ParserT::kOrder,
            []() -> std::unique_ptr<ITomlSystemConfigParser> {
                return std::make_unique<ParserT>();
            });
    }

    /// \brief Whether registration succeeded.
    ///
    /// \return true if the parser was successfully registered.
    bool registered() const noexcept {
        return registered_;
    }

  private:
    bool registered_{false};
};

/// \brief Static file-scope registrar for ITomlDocumentConfigParser subclasses.
///
/// Instantiate as a const file-scope object in a parser .cpp file:
/// \code{.cpp}
/// const TomlDocumentParserRegistration<MyDocumentParser> kRegisterMyParser;
/// \endcode
///
/// \tparam ParserT Concrete ITomlDocumentConfigParser subclass with static
///                 \c kName (string_view) and \c kOrder (int) members.
template <typename ParserT> class TomlDocumentParserRegistration {
  public:
    /// \brief Registers the parser factory with the singleton registry.
    TomlDocumentParserRegistration() {
        registered_ = TomlParserRegistry::instance().add_document_parser(
            ParserT::kName, ParserT::kOrder,
            []() -> std::unique_ptr<ITomlDocumentConfigParser> {
                return std::make_unique<ParserT>();
            });
    }

    /// \brief Whether registration succeeded.
    ///
    /// \return true if the parser was successfully registered.
    bool registered() const noexcept {
        return registered_;
    }

  private:
    bool registered_{false};
};

} // namespace hpactor::config
