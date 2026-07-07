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

#include <cstddef>
#include <cstdint>

namespace hpactor::python {

/// \brief Point-in-time snapshot of Python bridge queue depths and rejection
///        counters. Designed for lightweight metrics export and CLI inspection.
struct PythonQueueSnapshot final {
    /// Current number of pending dispatch envelopes.
    size_t dispatch_depth{0};

    /// Current number of pending commands from the interpreter.
    size_t command_depth{0};

    /// Current number of pending completions to return to the interpreter.
    size_t completion_depth{0};

    /// Cumulative number of dispatch envelopes rejected due to a full queue.
    uint64_t dispatch_rejected{0};

    /// Cumulative number of commands rejected due to a full queue.
    uint64_t command_rejected{0};

    /// Cumulative number of completions rejected due to a full queue.
    uint64_t completion_rejected{0};
};

} // namespace hpactor::python
