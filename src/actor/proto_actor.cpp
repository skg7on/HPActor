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

#include <hpactor/actor/proto_actor.hpp>

namespace hpactor {

proto_actor::proto_actor(ActorContext* ctx, ActorSystem& sys)
    : EventBasedActor(ctx, sys) {}

void proto_actor::initialize_proto_handlers() {
    if (handlers_initialized_) return;
    register_handlers();
    handlers_initialized_ = true;
}

void proto_actor::on_proto_message(TypeTag tag, const bytes& payload) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    auto it = proto_handlers_.find(tag);
    if (it == proto_handlers_.end()) {
        return; // No handler for this tag -- silently drop
    }

    ProtoHandler& handler = it->second;
    auto msg = handler.deserialize(payload);
    if (!msg) return; // Deserialization failed

    bytes response = handler.invoke(std::move(msg));
    (void)response;
}

void proto_actor::receive(MessageVariant&& msg) {
    if (!handlers_initialized_) {
        initialize_proto_handlers();
    }

    EventBasedActor::receive(std::move(msg));
}

} // namespace hpactor
