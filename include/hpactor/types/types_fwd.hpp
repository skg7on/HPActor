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

#include <hpactor/adt/id.hpp>
#include <hpactor/adt/tags.hpp>

namespace hpactor {

// Forward declarations only - no type aliases or definitions
//
// ActorId, MessageId, and AlarmHandle are type aliases (Id<Tag>), so they
// cannot be forward-declared; the necessary headers are included above.

struct TraceContext;
class error;
class Clock;
template <typename T> class Task;

} // namespace hpactor
