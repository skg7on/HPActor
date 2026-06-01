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

#include <hpactor/fault/fault_types.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace hpactor::fault {

/// \brief A named fault-injection site registered in the global registry.
///
/// Each \c FaultPoint describes one location in the codebase where a
/// \c FAULT_INJECT(path) macro can fire. The \c path uses dot-separated
/// hierarchical naming (e.g. \c "hpactor.mailbox.enqueue.fail").
struct FaultPoint {
    std::string path;   ///< Dot-separated hierarchical name of the injection
                        ///< site.
    FaultDomain domain; ///< Subsystem domain this site belongs to.
    std::string description; ///< Human-readable description of the injected
                             ///< fault.
};

/// \brief Global singleton registry of all \c FaultPoint sites.
///
/// Populated at static-init time by \c FaultPointRegistrar objects. Provides
/// lookup and prefix matching used by \c FaultController::check() to determine
/// whether an injection site falls within the active scope.
///
/// \note Thread safety: registration is single-threaded during static init.
///       Lookup and match methods are read-only and safe to call concurrently
///       from any thread once static init is complete.
class FaultPointRegistry {
  public:
    /// \brief Return the global singleton instance.
    static FaultPointRegistry& instance();

    /// \brief Register a fault point by path.
    ///
    /// \param[in] path Dot-separated hierarchical name.
    /// \param[in] domain Subsystem domain.
    /// \param[in] description Human-readable description.
    void
    register_point(std::string path, FaultDomain domain, std::string description);

    /// \brief Find a fault point by exact path match.
    ///
    /// \param[in] path Exact dot-separated path to search for.
    /// \return Pointer to the matching \c FaultPoint, or \c nullptr if not
    /// found.
    const FaultPoint* lookup(std::string_view path) const;

    /// \brief Check whether \p path matches \p prefix_pattern.
    ///
    /// A \p prefix_pattern of \c "*" matches everything. Otherwise the pattern
    /// is compared character-by-character; a \c '*' in the pattern acts as a
    /// wildcard suffix. Shorter patterns that do not end in \c '*' require an
    /// exact match.
    ///
    /// \param[in] path The full dot-separated path to test.
    /// \param[in] prefix_pattern The scope or prefix pattern to match against.
    /// \return \c true if \p path falls within \p prefix_pattern.
    bool
    matches_prefix(std::string_view path, std::string_view prefix_pattern) const;

    /// \brief Collect all registered fault points whose path matches \p
    /// prefix_pattern.
    ///
    /// \param[in] prefix_pattern Scope or prefix pattern.
    /// \param[out] out Vector to append matching \c FaultPoint pointers to.
    void collect_prefix(std::string_view prefix_pattern,
                        std::vector<const FaultPoint*>& out) const;

    /// \brief Return the full list of registered fault points.
    const std::vector<FaultPoint>& points() const noexcept {
        return points_;
    }

  private:
    FaultPointRegistry() = default;
    std::vector<FaultPoint> points_;
};

/// \brief RAII helper that registers a \c FaultPoint at static-init time.
///
/// Instantiate as a file-scope \c const object to self-register a fault point
/// before \c main(). The registration is append-only and never removed.
///
/// \note Instances are typically placed in anonymous namespaces inside
///       \c src/fault/fault_points.cpp.
struct FaultPointRegistrar {
    /// \brief Register a fault point with the global registry.
    ///
    /// \param[in] path Dot-separated hierarchical name.
    /// \param[in] domain Subsystem domain.
    /// \param[in] description Human-readable description.
    FaultPointRegistrar(std::string_view path, FaultDomain domain,
                        std::string_view description) {
        FaultPointRegistry::instance().register_point(std::string(path), domain,
                                                      std::string(description));
    }
};

} // namespace hpactor::fault
