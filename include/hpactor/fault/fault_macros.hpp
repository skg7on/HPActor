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

#include <hpactor/fault/fault_controller.hpp>
#include <hpactor/hpactor_config.hpp>
#include <hpactor/platform.hpp>

#if HPACTOR_ENABLE_FAULT_INJECTION
#    define FAULT_INJECT(path)                                                 \
        if (auto* _fc = ::hpactor::fault::FaultController::thread_local_instance(); \
            HPACTOR_UNLIKELY(_fc != nullptr && _fc->check(path)))
#else
#    define FAULT_INJECT(path) if (false)
#endif
