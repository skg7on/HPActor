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
#include <unordered_map>
#include <vector>

namespace hpactor {

// ── ReloadClass ─────────────────────────────────────────────────────────────

/// \brief Whether a config field can be changed at runtime.
enum class ReloadClass : uint8_t {
    Live,            ///< Can be changed at runtime without restart.
    RestartRequired, ///< Requires full system restart.
    Immutable,       ///< Cannot be changed after initial construction.
};

// ── ConfigPathId ────────────────────────────────────────────────────────────

/// \brief Unique path identifier for a config field.
///
/// Each field in the effective configuration has a unique path.
/// Paths are owned by subsystem parsers and never collide.
using ConfigPathId = uint32_t;
constexpr ConfigPathId kInvalidConfigPathId = 0;

// ── ConfigFieldDescriptor ───────────────────────────────────────────────────

/// \brief Descriptor for one config field: path, reload class, and optional
///        validation callback.
struct ConfigFieldDescriptor {
    ConfigPathId path{kInvalidConfigPathId};
    ReloadClass reload_class{ReloadClass::RestartRequired};
    const char* description{nullptr};
};

// ── ConfigFieldRegistry ─────────────────────────────────────────────────────

/// \brief Registry of reload descriptors owned by subsystem parsers.
///
/// Subsystem parsers register their fields' reload classes here during
/// static initialization via \c ConfigFieldRegistrar. Unknown fields
/// default to \c RestartRequired.
class ConfigFieldRegistry {
  public:
    /// \brief Register a field descriptor.
    /// \return false if the path is already registered (duplicate).
    bool register_field(ConfigFieldDescriptor desc) noexcept {
        auto [it, inserted] = fields_.try_emplace(desc.path, desc);
        return inserted;
    }

    /// \brief Find a descriptor by path.
    const ConfigFieldDescriptor* find(ConfigPathId path) const noexcept {
        auto it = fields_.find(path);
        return it != fields_.end() ? &it->second : nullptr;
    }

    /// \brief Number of registered fields.
    size_t size() const noexcept {
        return fields_.size();
    }

    /// \brief Get all registered descriptors.
    std::vector<ConfigFieldDescriptor> snapshot() const {
        std::vector<ConfigFieldDescriptor> result;
        result.reserve(fields_.size());
        for (const auto& [_, desc] : fields_) {
            result.push_back(desc);
        }
        return result;
    }

    /// \brief Singleton instance used by subsystem parsers during static init.
    static ConfigFieldRegistry& instance() noexcept {
        static ConfigFieldRegistry registry;
        return registry;
    }

  private:
    std::unordered_map<ConfigPathId, ConfigFieldDescriptor> fields_;
};

// ── ReloadReport ────────────────────────────────────────────────────────────

/// \brief Report produced by a reload operation.
struct ReloadReport {
    /// \brief Number of fields that were live-reloaded.
    uint32_t live_fields_applied{0};
    /// \brief Number of fields that required a restart (rejected).
    uint32_t restart_required_fields{0};
    /// \brief Whether any immutable fields were in the diff (always rejected).
    bool immutable_rejected{false};
    /// \brief Whether the reload was fully applied or rejected.
    bool fully_applied{false};
    /// \brief Human-readable summary.
    const char* summary{nullptr};
};

} // namespace hpactor
