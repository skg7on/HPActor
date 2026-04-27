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

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/core/proto_type_registry.hpp>
#include <hpactor/types/types.hpp>

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>

namespace hpactor {

// Internal handler storage -- type-erased to avoid template bloat in the map
struct ProtoHandler {
    std::string type_name;

    ProtoHandler() = default;
    ProtoHandler(ProtoHandler&&) = default;
    ProtoHandler& operator=(ProtoHandler&&) = default;
    ProtoHandler(const ProtoHandler&) = delete;
    ProtoHandler& operator=(const ProtoHandler&) = delete;

    // Deserialize bytes into a shared_ptr<void> holding the concrete protobuf type
    std::function<std::shared_ptr<void>(const bytes&)> deserialize;

    // Invoke the handler with a deserialized message.
    // Returns serialized response bytes (empty for fire-and-forget).
    std::function<bytes(std::shared_ptr<void>)> invoke;
};

// proto_actor -- base class for protobuf-native actors.
//
// Users override register_handlers() and call on<MsgT>() / on_request<ReqT,ResT>()
// to register handlers for their protobuf message types.
class proto_actor : public EventBasedActor {
public:
    proto_actor(ActorContext* ctx, ActorSystem& sys);

    // Register a fire-and-forget handler for a protobuf message type
    template<typename ProtoMsgT>
    void on(std::function<void(const ProtoMsgT&)> handler) {
        TypeTag tag = type_tag_for<ProtoMsgT>();
        auto handler_ptr = std::make_shared<
            std::function<void(const ProtoMsgT&)>>(std::move(handler));

        ProtoHandler entry;
        entry.type_name = ProtoMsgT().GetTypeName();
        entry.deserialize = [](const bytes& data) -> std::shared_ptr<void> {
            auto msg = std::make_shared<ProtoMsgT>();
            if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return nullptr;
            }
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> bytes {
            auto& msg = *static_cast<ProtoMsgT*>(raw.get());
            (*handler_ptr)(msg);
            return {}; // no response
        };

        proto_handlers_[tag] = std::move(entry);
    }

    // Register a request-response handler for protobuf types
    template<typename ReqT, typename ResT>
    void on_request(std::function<ResT(const ReqT&)> handler) {
        TypeTag tag = type_tag_for<ReqT>();
        auto handler_ptr = std::make_shared<
            std::function<ResT(const ReqT&)>>(std::move(handler));

        ProtoHandler entry;
        entry.type_name = ReqT().GetTypeName();
        entry.deserialize = [](const bytes& data) -> std::shared_ptr<void> {
            auto msg = std::make_shared<ReqT>();
            if (!msg->ParseFromArray(data.data(), static_cast<int>(data.size()))) {
                return nullptr;
            }
            return msg;
        };
        entry.invoke = [handler_ptr](std::shared_ptr<void> raw) -> bytes {
            auto& req = *static_cast<ReqT*>(raw.get());
            ResT res = (*handler_ptr)(req);
            bytes result(res.ByteSizeLong());
            res.SerializeToArray(result.data(),
                                 static_cast<int>(result.size()));
            return result;
        };

        proto_handlers_[tag] = std::move(entry);
    }

    // Dispatch an incoming protobuf message by TypeTag
    void on_proto_message(TypeTag tag, const bytes& payload);

    // Check if this actor can handle a given TypeTag
    [[nodiscard]] bool handles(TypeTag tag) const {
        return proto_handlers_.find(tag) != proto_handlers_.end();
    }

protected:
    // Users override this to call on<T>() / on_request<ReqT,ResT>()
    virtual void register_handlers() = 0;

    // Called by the framework after construction to set up handlers
    void initialize_proto_handlers();

    void receive(MessageVariant&& msg) override;

    // Get TypeTag for a protobuf type from the system registry
    template<typename ProtoMsgT>
    TypeTag type_tag_for() const {
        return system().proto_registry().lookup<ProtoMsgT>();
    }

private:
    bool handlers_initialized_ = false;
    std::unordered_map<TypeTag, ProtoHandler> proto_handlers_;
};

} // namespace hpactor
