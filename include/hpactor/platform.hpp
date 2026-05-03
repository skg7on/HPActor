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

#ifdef __linux__
#    define HPACTOR_PLATFORM_LINUX 1
#elif defined(__APPLE__)
#    define HPACTOR_PLATFORM_MACOS 1
#else
#    define HPACTOR_PLATFORM_UNKNOWN 1
#endif

namespace hpactor {
using byte_t = unsigned char;

inline constexpr size_t default_mailbox_capacity = 1024;
} // namespace hpactor
