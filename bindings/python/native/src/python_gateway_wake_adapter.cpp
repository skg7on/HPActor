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

#include <hpactor/python/python_gateway_wake_adapter.hpp>

#include <hpactor/actor/actor_system.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor::python {

PythonGatewayWakeAdapter::PythonGatewayWakeAdapter(ActorSystem& system,
                                                   ActorAddress gateway) noexcept
    : system_(system), gateway_(gateway) {}

GatewayWakePort PythonGatewayWakeAdapter::port() noexcept {
    return {this, &PythonGatewayWakeAdapter::wake};
}

bool PythonGatewayWakeAdapter::wake(void* context) noexcept {
    auto* self = static_cast<PythonGatewayWakeAdapter*>(context);
    TypedMessage message(kPythonWakeupTag, StreamBuffer{});
    return self->system_
        .deliver_with_result(self->gateway_.id, std::move(message))
        .accepted();
}

} // namespace hpactor::python
