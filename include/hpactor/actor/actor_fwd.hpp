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

#include "../types/types_fwd.hpp"

namespace hpactor {

class AbstractActor;
class LocalActor;
class EventBasedActor;
class BlockingActor;
class ScopedActor;

class DaemonActor;
class PollingActor;
class DenseComputingActor;
class ExternalMsgGatewayActor;
class HTTPServerActor;

template <typename... Signatures> class TypedEventBasedActor;

template <typename T> class StatefulActor;

class Actor;
class ActorRef;

} // namespace hpactor