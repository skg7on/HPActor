#pragma once

#include <hpactor/actor/event_based_actor.hpp>
#include <hpactor/actor/behavior.hpp>
#include <hpactor/actor/system/actor_system.hpp>
#include <apps/name_resolution_mesh/messages.hpp>

#include <iostream>
#include <chrono>

namespace apps::name_resolution_mesh {

/// Base service actor. Handles:
///   - Ping (kPingTag): fire-and-forget, logs receipt
///   - PingRequest (kPingRequestTag): request-response, replies with
///     PongResponse carrying node identity and timestamp for verification
class ServiceActor : public hpactor::EventBasedActor {
public:
    ServiceActor(hpactor::ActorContext* ctx, hpactor::ActorSystem& sys,
                 std::string node_name, std::string service_name)
        : EventBasedActor(ctx, sys)
        , node_name_(std::move(node_name))
        , service_name_(std::move(service_name))
    {
        become(make_behavior());
    }

    const std::string& service_name() const { return service_name_; }
    const std::string& node_name() const { return node_name_; }

protected:
    hpactor::Behavior make_behavior() override {
        return hpactor::Behavior{[this](hpactor::TypedMessage& msg) {
            if (msg.type_id() == kPingTag) {
                std::cout << "  [" << node_name_ << ":" << service_name_
                          << "] received Ping (fire-and-forget)" << std::endl;
            } else if (msg.type_id() == kPingRequestTag) {
                std::cout << "  [" << node_name_ << ":" << service_name_
                          << "] received PingRequest, replying PongResponse"
                          << std::endl;
                PongResponse pong;
                pong.node_id = static_cast<uint32_t>(id().value());
                pong.service_name = service_name_;
                pong.timestamp_ns =
                    std::chrono::steady_clock::now().time_since_epoch().count();
                context()->reply(hpactor::TypedMessage(
                    kPingRequestTag, encode_pong_response(pong)));
            }
        }};
    }

    std::string node_name_;
    std::string service_name_;
};

// Public type aliases — each role gets its own named type for clarity
using AuthServiceActor      = ServiceActor;
using PaymentServiceActor   = ServiceActor;
using InventoryServiceActor = ServiceActor;

}  // namespace apps::name_resolution_mesh
