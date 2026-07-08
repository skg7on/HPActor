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
    ActorAddress actor;
    uint64_t generation{0};
    std::string detail_json;
    bool timed_out{false};
};

/// \brief Bounded asynchronous inspection service for Python actors.
///
/// Owns at most completion_queue_capacity pending entries. inspect() is
/// called only from the CLI/management thread. Late results are counted stale.
class PythonInspectionService final {
  public:
    explicit PythonInspectionService(uint32_t max_pending) noexcept;

    /// \brief Request an inspection of an actor.
    ///
    /// \param[in] actor The actor to inspect.
    /// \param[in] generation The expected generation.
    /// \param[in] timeout Maximum wait time.
    /// \return The result, or an error (timeout, not found, etc.).
    [[nodiscard]] result<PythonInspectResult>
    inspect(ActorAddress actor, uint64_t generation,
            std::chrono::milliseconds timeout) noexcept;

    /// \brief Complete a pending inspection with a result.
    void complete(PythonInspectResult result) noexcept;

    /// \brief Cancel all pending inspections.
    void cancel_all(error reason) noexcept;

    /// \brief Number of pending inspections.
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
