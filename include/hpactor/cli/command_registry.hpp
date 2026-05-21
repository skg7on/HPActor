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

#include <hpactor/cli/command_context.hpp>
#include <hpactor/types/types.hpp>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace hpactor {
namespace cli {

/// \brief Abstract command registered via a static file-scope registrar.
///
/// Subclasses declare a path (with "/"-separated segments and \c <param>
/// placeholders for parameter nodes), help text, and ordering priority.
/// Every subclass is instantiated through a \c CommandRegistration<T>
/// file-scope object, which registers the command before \c main().
///
/// Path examples: "help", "actor/<id>/show", "system/drain/status"
///
/// \note Registration-time only. The objects are owned by the
///       \c CommandRegistry singleton for the lifetime of the process.
class ICommand {
  public:
    virtual ~ICommand() = default;

    /// \brief Slash-separated command path.
    ///
    /// Segments wrapped in \c <angle> \c brackets denote parameter nodes
    /// that match any runtime token and capture the value into
    /// \c CommandContext::params under the same bracketed name.
    ///
    /// \return A non-empty path, e.g. "actor/<id>/show".
    virtual std::string_view path() const noexcept = 0;

    /// \brief One-line description shown in help output.
    virtual std::string_view help_text() const noexcept = 0;

    /// \brief Registration ordering for deterministic tree assembly.
    ///
    /// Lower values are mounted first. Commands with equal order are
    /// sorted by path as a secondary key.
    ///
    /// \return A non-negative integer.
    virtual int order() const noexcept = 0;

    /// \brief Execute the command in response to user input.
    ///
    /// Called on the CLI daemon's dedicated thread. The implementation
    /// may block synchronously (e.g. for actor-inspect round-trips)
    /// because no other actor shares this thread.
    ///
    /// \param[in,out] ctx Parsed tokens, flags, captured parameters,
    ///                    and output formatter.
    /// \return A \c result<void> indicating success or an error code.
    /// \note Thread affinity: runs on the CLI daemon thread.
    virtual result<void> execute(CommandContext& ctx) const = 0;
};

/// \brief Singleton registry for CLI commands.
///
/// Commands are registered before \c main() via \c CommandRegistration<T>
/// file-scope objects. The registry is read once during \c CliActor
/// construction to build the command tree; no new commands are added
/// after startup.
///
/// \note Single-threaded registration phase (static init), read-only
///       thereafter. No mutex needed.
class CommandRegistry {
  public:
    /// \brief Access the singleton instance.
    ///
    /// \return Reference to the process-global registry.
    static CommandRegistry& instance();

    /// \brief Register a command (called by \c CommandRegistration<T>).
    ///
    /// \param[in] cmd The command object; ownership is transferred.
    void add(std::unique_ptr<ICommand> cmd);

    /// \brief Immutable snapshot of all registered commands.
    ///
    /// \return A reference to the internal vector. Stable for the
    ///         lifetime of the process once static init completes.
    const std::vector<std::unique_ptr<ICommand>>& commands() const;

  private:
    CommandRegistry() = default;
    std::vector<std::unique_ptr<ICommand>> commands_;
};

/// \brief Static file-scope registrar for auto-registering CLI commands.
///
/// Instantiate as a \c const file-scope object:
/// \code{.cpp}
/// const CommandRegistration<MyCommand> kRegisterMyCommand;
/// \endcode
///
/// The constructor registers a heap-allocated instance of \c CommandT
/// with \c CommandRegistry::instance() so the command is available
/// before \c main() and requires no edits to \c CliActor.
///
/// \tparam CommandT Concrete subclass of \c ICommand with a default
///                 constructor.
template <typename CommandT> class CommandRegistration {
  public:
    CommandRegistration() {
        CommandRegistry::instance().add(std::make_unique<CommandT>());
    }
};

/// \brief Split a command path string into segment tokens.
///
/// Paths use "/" as a separator. Empty segments (leading, trailing, or
/// consecutive slashes) are discarded.
///
/// \param[in] path A slash-separated path, e.g. "actor/<id>/show".
/// \return Ordered vector of non-empty segments.
inline std::vector<std::string> parse_command_path(std::string_view path) {
    std::vector<std::string> segs;
    size_t start = 0;
    while (start < path.size()) {
        size_t slash = path.find('/', start);
        std::string_view seg = (slash == std::string_view::npos)
                                   ? path.substr(start)
                                   : path.substr(start, slash - start);
        if (!seg.empty())
            segs.emplace_back(seg);
        if (slash == std::string_view::npos)
            break;
        start = slash + 1;
    }
    return segs;
}

/// \brief Test whether a path segment represents a parameter placeholder.
///
/// A parameter segment is any non-empty string enclosed in angle brackets,
/// e.g. \c "<id>", \c "<actor_id>", \c "<filter>".
///
/// \param[in] seg A single path segment.
/// \retval true  The segment is a parameter placeholder.
/// \retval false The segment is a literal keyword.
inline bool is_param_segment(const std::string& seg) {
    return !seg.empty() && seg.front() == '<' && seg.back() == '>';
}

} // namespace cli
} // namespace hpactor
