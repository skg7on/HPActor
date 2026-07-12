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

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>

namespace hpactor::python {

/// \brief Result of an actor inspection.
struct PythonInspectResult final {
    ActorAddress actor;           ///< The inspected actor's address.
    uint64_t generation{0};       ///< Actor generation at inspection time.
    std::string detail_json;      ///< JSON-serialized state payload (max 16 KiB).
    bool timed_out{false};        ///< Whether the inspection timed out before completion.
};

/// \brief Bounded asynchronous inspection service for Python actors.
///
/// Owns at most completion_queue_capacity pending entries. inspect() is
/// called only from the CLI/management thread. Late results are counted stale.
class PythonInspectionService final {
  public:
    /// \brief Construct the inspection service with a maximum pending capacity.
    ///
    /// \param[in] max_pending Maximum number of concurrent pending inspections
    ///                        (1..4096).
    explicit PythonInspectionService(uint32_t max_pending) noexcept;

    /// \brief Request an inspection of an actor.
    ///
    /// \param[in] actor The actor to inspect.
    /// \param[in] generation The expected generation. Mismatch returns an error.
    /// \param[in] timeout Maximum wait time for the inspector response.
    /// \return The result, or an error (timeout, not found, generation
    ///         mismatch, or queue full).
    /// \note Thread safety: called only from the CLI/management thread.
    [[nodiscard]] result<PythonInspectResult>
    inspect(ActorAddress actor, uint64_t generation,
            std::chrono::milliseconds timeout) noexcept;

    /// \brief Complete a pending inspection with a result.
    ///
    /// Matched by actor address and generation to the pending inspection.
    ///
    /// \param[in] result The completed inspection result.
    /// \note Thread safety: called from the Python runtime thread.
    void complete(PythonInspectResult result) noexcept;

    /// \brief Cancel all pending inspections with the given error reason.
    ///
    /// \param[in] reason The error to attach to each cancelled inspection.
    /// \note Thread safety: safe to call from any thread.
    void cancel_all(error reason) noexcept;

    /// \brief Number of pending inspections.
    ///
    /// \return The count of inspections that have not yet been completed.
    /// \note Thread safety: safe to call from any thread; inherently racy.
    [[nodiscard]] size_t pending_count() const noexcept;

  private:
    struct PendingInspection {
        PythonInspectResult result;
        bool completed{false};
    };

    uint32_t max_pending_;
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, PendingInspection> pending_;
    uint64_t next_token_{1};
};

} // namespace hpactor::python
