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

#include <hpactor/python/python_bridge_types.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor::python {

/// \brief Encode a Python actor command into a deterministic protobuf payload.
///
/// The payload is encoded with \c PbPythonActorCommand. The caller is
/// responsible for delivering the resulting bytes through a protected
/// \c kPythonActorCommandTag envelope.
///
/// \param[in] command The command to encode.
/// \return The serialized protobuf bytes, or an error result.
[[nodiscard]] result<StreamBuffer>
encode_actor_command(const PythonCommand& command) noexcept;

/// \brief Decode a protobuf payload back into a Python actor command.
///
/// Validates the version field, all bounded fields (payload ≤ 16 MiB,
/// detail ≤ 16 KiB, name ≤ 255 bytes), valid command kind, valid application
/// tag range, and valid addresses. Returns an error on any violation.
///
/// \param[in] buffer The serialized protobuf bytes.
/// \return The decoded command, or an error result.
[[nodiscard]] result<PythonCommand>
decode_actor_command(const StreamBuffer& buffer) noexcept;

/// \brief Encode a Python actor failure into a deterministic protobuf payload.
///
/// Bounds-checked: exception_type ≤ 255 bytes, message ≤ 4 KiB,
/// traceback ≤ 16 KiB.
///
/// \param[in] address  The failed actor's address.
/// \param[in] generation The failed actor's generation.
/// \param[in] exception_type Python exception type name.
/// \param[in] message       Exception message.
/// \param[in] traceback     Exception traceback string.
/// \param[in] sequence      Sequence number of the failing turn.
/// \return The serialized protobuf bytes, or an error result.
[[nodiscard]] result<StreamBuffer>
encode_actor_failed(const ActorAddress& address, uint64_t generation,
                    const std::string& exception_type, const std::string& message,
                    const std::string& traceback, uint64_t sequence) noexcept;

/// \brief Decode a protobuf payload back into actor-failure metadata.
///
/// \param[in] buffer The serialized protobuf bytes.
/// \param[out] address   The failed actor's address.
/// \param[out] generation The failed actor's generation.
/// \param[out] exception_type Python exception type name.
/// \param[out] message       Exception message.
/// \param[out] traceback     Exception traceback string.
/// \param[out] sequence      Sequence number of the failing turn.
/// \return ok() on success, or an error result.
[[nodiscard]] result<void>
decode_actor_failed(const StreamBuffer& buffer, ActorAddress& address,
                    uint64_t& generation, std::string& exception_type,
                    std::string& message, std::string& traceback,
                    uint64_t& sequence) noexcept;

} // namespace hpactor::python
