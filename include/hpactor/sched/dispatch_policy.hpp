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

namespace hpactor::sched {

using DispatchPolicy = hpactor::DispatchPolicy;

struct DispatchHints {
    int cpu_affinity = -1;
    uint32_t pool_size = 1;
    uint8_t priority = 0;
};

} // namespace hpactor::sched
