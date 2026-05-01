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

#include <hpactor/actor_context.hpp>
#include <hpactor/core/actor_system.hpp>
#include <hpactor/ref/actor_proxy.hpp>

#include <google/protobuf/message.h>

namespace hpactor {

ActorContext::ActorContext(Actor owner, ActorSystem* system)
    : owner_(std::move(owner)), system_(system) {}

ActorContext::~ActorContext() = default;

ActorRef ActorContext::resolve(const ActorAddress& target) {
    // 1. Check cache (hot path)
    auto cached = ref_cache_.get(target.id);
    if (cached.has_value()) {
        return *cached;
    }

    // 2. Resolve system pointer
    auto system = system_ ? system_ : (owner_ ? &owner_.get()->system() : nullptr);
    if (!system) {
        return ActorRef{};
    }

    // 3. If target endpoint differs from our own, it's a remote actor.
    //    Skip local lookup — actor IDs are only unique within a system.
    if (!(target.endpoint == system->endpoint())) {
        ActorProxy proxy(target, system);
        ActorRef ref(std::move(proxy));
        // Only cache if transport was resolved successfully
        if (ref.get_proxy() && ref.get_proxy()->transport()) {
            ref_cache_.put(target.id, ref);
        }
        return ref;
    }

    // 4. Local endpoint: check local actors
    auto actor = system->get_actor(target.id);
    if (actor) {
        ActorRef ref{Actor(actor)};
        ref_cache_.put(target.id, ref);
        return ref;
    }

    return ActorRef{};
}

void ActorContext::send(ActorRef& target, TypedMessage msg) {
    // Stamp sender identity for reply tracking
    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    target.send(target.address(), std::move(msg));
}

void ActorContext::send(const ActorAddress& target, TypedMessage msg) {
    auto ref = resolve(target);
    if (ref) {
        send(ref, std::move(msg));
    }
    // If resolve failed (no transport, no local actor), silently drop.
    // Matches async messaging semantics — fire and forget.
}

void ActorContext::send(const ActorAddress& target, TypeTag tag,
                        const google::protobuf::Message& proto_msg) {
    TypedMessage msg(tag, proto_msg);
    send(target, std::move(msg));
}

void ActorContext::send_with_priority(const ActorAddress& target, TypedMessage msg,
                                      uint8_t priority, int64_t deadline_ns) {
    auto ref = resolve(target);
    if (!ref) return;

    if (owner_) {
        msg.set_sender_address(owner_.address());
    }

    if (ref.is_local()) {
        auto system = owner_ ? &owner_.get()->system() : system_;
        if (system) {
            system->deliver_local(target.id, std::move(msg), priority, deadline_ns);
        }
    } else {
        ref.send(target, std::move(msg));
    }
}

void ActorContext::reply(TypedMessage msg) {
    if (current_sender_.id != ActorId{0}) {
        send(current_sender_, std::move(msg));
    }
}

void ActorContext::reply(TypeTag tag, const google::protobuf::Message& proto_msg) {
    TypedMessage msg(tag, proto_msg);
    reply(std::move(msg));
}

void ActorContext::reply_with_error(error err) {
    if (current_sender_.id == ActorId{0}) return;

    // Wire format: [4 bytes: error code BE][error message string]
    // A protobuf error message can replace this payload later without
    // changing the TypeTag or dispatch path.
    bytes payload;
    uint32_t code = err.code();
    payload.push_back(static_cast<uint8_t>((code >> 24) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code >> 16) & 0xFF));
    payload.push_back(static_cast<uint8_t>((code >> 8) & 0xFF));
    payload.push_back(static_cast<uint8_t>(code & 0xFF));
    const auto& msg = err.message();
    payload.insert(payload.end(), msg.begin(), msg.end());

    TypedMessage error_msg(TypeTag::ErrorMsg, std::move(payload));
    send(current_sender_, std::move(error_msg));
}

void ActorContext::schedule(std::chrono::milliseconds delay, TypedMessage msg) {
    // TODO: schedule message via actor system's clock/alarm mechanism
    (void)delay;
    (void)msg;
}

std::vector<Actor> ActorContext::children() const {
    return children_;
}

void ActorContext::add_child(Actor child) {
    children_.push_back(std::move(child));
}

void ActorContext::remove_child(Actor child) {
    for (auto it = children_.begin(); it != children_.end(); ++it) {
        if (it->address() == child.address()) {
            children_.erase(it);
            return;
        }
    }
}

std::vector<ActorRef> ActorContext::remote_children() const {
    return remote_children_;
}

void ActorContext::add_remote_child(ActorRef child) {
    remote_children_.push_back(std::move(child));
}

std::vector<ActorAddress> ActorContext::linked_actors() const {
    return linked_;
}

void ActorContext::monitor(const ActorAddress& target) {
    add_monitored(target);
}

RpcFuture<bytes>
ActorContext::rpc(const ActorAddress& target, const bytes& encoded_request,
                  std::chrono::milliseconds timeout_ms) {
    return system_->rpc_channel().call_raw(target, encoded_request, timeout_ms);
}

RpcFuture<bytes>
ActorContext::http_get(const std::string& url,
                       std::vector<net::HttpHeader> headers) {
    return system_->http_client().get(url, std::move(headers));
}

RpcFuture<bytes>
ActorContext::http_post(const std::string& url, bytes body,
                         std::vector<net::HttpHeader> headers) {
    return system_->http_client().post(url, std::move(body), std::move(headers));
}

RpcFuture<bytes>
ActorContext::http_put(const std::string& url, bytes body,
                        std::vector<net::HttpHeader> headers) {
    return system_->http_client().put(url, std::move(body), std::move(headers));
}

RpcFuture<bytes>
ActorContext::http_delete(const std::string& url,
                           std::vector<net::HttpHeader> headers) {
    return system_->http_client().del(url, std::move(headers));
}

RpcFuture<bytes>
ActorContext::http_request(net::HttpMethod method, const std::string& url,
                            std::vector<net::HttpHeader> headers, bytes body) {
    return system_->http_client().request(method, url, std::move(headers),
                                           std::move(body));
}

} // namespace hpactor
