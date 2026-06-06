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

#include <hpactor/spawn.hpp>

namespace hpactor {
// AsyncActor is now a type alias for RequestHandle<ActorRef> (defined in
// spawn.hpp). All prior method implementations on the AsyncActor class
// (get, ready, cancel, set_response) have been replaced by RequestHandle<T>
// methods. This translation unit exists for backward ABI compatibility.
} // namespace hpactor
