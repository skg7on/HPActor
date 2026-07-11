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

#include <hpactor/types/types.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace hpactor::config {
struct ActorDef;
} // namespace hpactor::config

namespace hpactor::python {

/// \brief Classification of a configured actor in a topology.
enum class ConfiguredActorKind : uint8_t {
    Cpp = 0,    ///< The actor is a C++ actor resolved via ActorFactoryRegistry.
    Python = 1, ///< The actor is a Python actor resolved via factory manifest.
};

/// \brief Parsed reference to a Python module and qualified class name.
struct PythonBehaviorRef final {
    std::string module;   ///< Absolute dotted module name (max 255 bytes).
    std::string qualname; ///< Qualified class name within the module (max 255 bytes).
};

/// \brief A prepared specification for one configured actor from a topology.
///
/// Produced by PythonTopologyPreparer during side-effect-free parsing.
struct PreparedActorSpec final {
    size_t topology_index{0};                 ///< Position in the topology model.
    ConfiguredActorKind kind{ConfiguredActorKind::Cpp}; ///< C++ or Python.
    std::optional<PythonBehaviorRef> python;   ///< Present only when kind == Python.
    uint64_t args_fingerprint{0};             ///< FNV-1a over sorted args.
    uint64_t factory_token{0};                ///< Bound token from factory manifest.
};

/// \brief Phase in the topology lifecycle.
enum class PythonTopologyPhase : uint8_t {
    Idle = 0,
    Parse = 1,
    Policy = 2,
    Import = 3,
    ClassResolution = 4,
    ClassValidation = 5,
    ConstructorBinding = 6,
    NativePrepare = 7,
    ActorStart = 8,
    Commit = 9,
    Rollback = 10,
};

/// \brief Bounded structured error info for topology failures.
struct PythonTopologyErrorInfo final {
    PythonTopologyPhase phase{PythonTopologyPhase::Idle};
    std::string actor_id;
    std::string behavior;
    uint32_t error_code{0};
    std::string detail;
    uint32_t rollback_error_bits{0};
};

/// \brief Parse a Python behavior reference string.
///
/// The grammar is exactly \c "python:<module>:<qualname>".
/// \c <module> and every dotted segment must match \c [A-Za-z_][A-Za-z0-9_]*.
/// \c module and \c qualname are each limited to 255 UTF-8 bytes; the complete
/// behavior value is limited to 518 bytes.
///
/// \param[in] behavior The behavior string from an \c ActorDef.
/// \return A \c PythonBehaviorRef on success, or \c std::nullopt if the
///         string does not start with \c "python:" or is malformed.
std::optional<PythonBehaviorRef>
parse_python_behavior_ref(std::string_view behavior) noexcept;

/// \brief Validate that an actor's arguments conform to Python binding bounds.
///
/// Checks:
/// - Every key is a valid Python identifier (starts with \c [A-Za-z_],
///   followed by \c [A-Za-z0-9_]*).
/// - No key starts with \c "__hpactor_".
/// - Each key is at most 128 bytes.
/// - Each value is at most 4096 bytes.
/// - At most 128 arguments.
/// - Combined key+value bytes at most 64 KiB.
///
/// \param[in] def The actor definition to validate.
/// \return \c ok() on success, or an error describing the first violation.
result<void> validate_python_actor_args(const config::ActorDef& def) noexcept;

/// \brief Compute a deterministic FNV-1a 64-bit fingerprint of actor arguments.
///
/// Hashes sorted, length-prefixed key/value byte pairs so the fingerprint is
/// independent of \c unordered_map iteration order.
///
/// \param[in] def The actor definition whose args should be fingerprinted.
/// \return A non-zero FNV-1a 64-bit hash.
uint64_t fingerprint_python_actor_args(const config::ActorDef& def) noexcept;

} // namespace hpactor::python
