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

#include "cluster_runtime_impl.hpp"
#include <arpa/inet.h>
#include <hpactor/runtime/actor_system_impl.hpp>
#include <hpactor/runtime/network_runtime.hpp>

#include <hpactor/actor/system/actor_system.hpp>
#include <hpactor/cluster/name/name_resolver.hpp>
#include <hpactor/config/name_resolution_config.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/msg/typed_message.hpp>

namespace hpactor {

void ActorSystem::enable_cluster(const std::string& node_id) {
    ClusterRuntimeDependencies deps;
    deps.node_id = node_id;

    impl_->cluster_ = create_cluster_runtime(deps, nullptr);
    if (impl_->cluster_) {
        (void)impl_->cluster_->start();
    }

    // ── Distributed name resolution subsystem ─────────────────────────────
    // Wired here so hpactor_lib (actor_system.cpp) never directly calls
    // NameResolver::resolve() — that avoids a circular link dependency
    // (hpactor_lib ↔ hpactor_cluster).  resolve_name_fn is a std::function
    // bridge set by this hpactor_cluster TU and invoked from hpactor_lib.

    config::NameResolutionConfig nr_cfg;
    nr_cfg.enabled = true;

    // Early exit if networking or discovery are unavailable.
    if (!impl_->network_ || !impl_->network_->discovery()) {
        return;
    }

    impl_->name_directory_ = std::make_unique<cluster::name::NameDirectory>();
    impl_->name_resolve_cache_ =
        std::make_unique<cluster::name::NameResolveCache>();

    // ── Outbound port: wired to transport ─────────────────────────────────
    // Uses Impl* as context — the send callback extracts transport and local
    // endpoint from it.  No heap allocation, no std::function.
    cluster::name::OutboundNameQueryPort outbound_port;
    outbound_port.context = impl_.get();
    outbound_port.send = [](void* ctx, EndPoint target, TypedMessage msg) {
        auto* impl = static_cast<ActorSystem::Impl*>(ctx);
        auto* transport = impl->network_->transport();
        if (!transport)
            return;

        // Ensure a TCP connection exists before sending.
        // ConnectionPool::try_send() silently queues when no connection
        // exists, so on first use we must explicitly connect.
        if (!transport->is_connected(target)) {
            std::string host;
            uint16_t port = 0;
            if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&target)) {
                uint32_t addr_nw = ipv4->addr;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
                addr_nw = __builtin_bswap32(addr_nw);
#endif
                char buf[INET_ADDRSTRLEN];
                struct in_addr in{};
                in.s_addr = addr_nw;
                inet_ntop(AF_INET, &in, buf, sizeof(buf));
                host = buf;
                port = ipv4->port();
            } else if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&target)) {
                char buf[INET6_ADDRSTRLEN];
                inet_ntop(AF_INET6, ipv6->addr.data(), buf, sizeof(buf));
                host = buf;
                port = ipv6->port();
            }
            if (!host.empty() && port > 0) {
                (void)transport->connect(target, host, port);
            }
        }

        // Build ActorMsgFrame protobuf: sender=local node, receiver=target.
        ActorAddress sender_addr{impl->core.endpoint, ActorType{0}, ActorId{0}, 0};
        ActorAddress receiver_addr{target, ActorType{0}, ActorId{0}, 0};

        net::WireFrame frame;
        net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_sender(),
                      sender_addr);
        net::to_proto(frame.pb_envelope.mutable_data_frame()->mutable_receiver(),
                      receiver_addr);
        frame.pb_envelope.mutable_data_frame()->set_type_tag(
            static_cast<uint32_t>(msg.type_id()));
        frame.pb_envelope.mutable_data_frame()->set_payload(
            reinterpret_cast<const char*>(msg.payload().data()),
            msg.payload().size());

        auto encoded = frame.encode();
        (void)transport->try_send(receiver_addr, encoded);
    };

    // Inbound port: wired to the NameResolver so incoming name-protocol
    // frames (tags 0x80-0x84) are dispatched via InboundFrameRouter.
    cluster::name::InboundNamePort inbound_port;
    inbound_port.context = nullptr; // set after NameResolver construction

    impl_->name_resolver = std::make_unique<cluster::name::NameResolver>(
        *impl_->name_directory_, *impl_->network_->discovery(),
        *impl_->name_resolve_cache_, nr_cfg, impl_->core.endpoint,
        outbound_port, inbound_port);

    // Wire the inbound port — InboundFrameRouter dispatches name-protocol
    // frames to the NameResolver via these function pointers.
    inbound_port.context = impl_->name_resolver.get();
    inbound_port.on_register_request = [](void* ctx, EndPoint from,
                                          std::string_view name,
                                          ActorAddress addr, uint64_t gen) {
        static_cast<cluster::name::NameResolver*>(ctx)->on_name_register_request(
            from, name, addr, gen);
    };
    inbound_port.on_register_response = [](void* ctx, EndPoint from,
                                           uint32_t result_code) {
        static_cast<cluster::name::NameResolver*>(ctx)->on_name_register_response(
            from, result_code);
    };
    inbound_port.on_resolve_query = [](void* ctx, EndPoint from,
                                       std::string_view name) {
        static_cast<cluster::name::NameResolver*>(ctx)->on_name_resolve_query(
            from, name);
    };
    inbound_port.on_resolve_response = [](void* ctx, EndPoint from, bool found,
                                          uint64_t actor_id,
                                          std::string_view endpoint_str,
                                          uint64_t generation) {
        static_cast<cluster::name::NameResolver*>(ctx)->on_name_resolve_response(
            from, found, actor_id, endpoint_str, generation);
    };
    inbound_port.on_unregister_request = [](void* ctx, EndPoint from,
                                            std::string_view name, uint64_t gen) {
        static_cast<cluster::name::NameResolver*>(ctx)->on_name_unregister_request(
            from, name, gen);
    };
    if (impl_->inbound_frame_router_) {
        impl_->inbound_frame_router_->set_name_port(inbound_port);
    }

    // Wire the registration port — ActorDirectory notifies NameResolver
    // when names are published (register) or erased (unregister).
    cluster::name::NameRegistrationPort reg_port;
    reg_port.context = impl_->name_resolver.get();
    reg_port.on_register = [](void* ctx, std::string_view name,
                              ActorAddress addr, uint64_t gen) {
        static_cast<cluster::name::NameResolver*>(ctx)->on_local_register(
            name, addr, gen);
    };
    reg_port.on_unregister = [](void* ctx, std::string_view name) {
        static_cast<cluster::name::NameResolver*>(ctx)->on_local_unregister(name);
    };
    impl_->actors.directory.set_name_registration_port(reg_port);

    // Bind the bridge so ActorSystem::resolve_actor() can query the
    // name resolver without linking hpactor_lib against NameResolver.
    impl_->resolve_name_fn = [nr = impl_->name_resolver.get()](std::string_view name) {
        return nr->resolve(name);
    };
}

} // namespace hpactor
