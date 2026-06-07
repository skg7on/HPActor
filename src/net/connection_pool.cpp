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

#include <hpactor/actor/spawn.hpp>
#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/connection_pool.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/fault/fault_macros.hpp>
#include <hpactor/log/logger.hpp>
#include <hpactor/messages.pb.h>

namespace hpactor {

namespace net {

ConnectionPool::ConnectionPool(EndPoint remote_endpoint,
                               const PoolConfig& config, EventLoop* loop)
    : remote_endpoint_(remote_endpoint), config_(config), loop_(loop),
      outbound_queue_(config.outbound_limits),
      circuit_breaker_(config.circuit_breaker_cfg) {}

ConnectionPool::~ConnectionPool() {
    abort();
}

ConnectionPtr ConnectionPool::get_connection() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_connections_.empty()) {
        return nullptr;
    }
    auto index = next_index_.fetch_add(1) % active_connections_.size();
    return active_connections_[index];
}

void ConnectionPool::send(const ActorAddress& target, const StreamBuffer& encoded) {
    FAULT_INJECT("hpactor.connection_pool.send.drop") {
        return;
    }
    if (shutting_down_.load()) {
        return;
    }

    if (!circuit_breaker_.allow_send()) {
        return;
    }

    ConnectionPtr conn = get_connection();
    if (conn) {
        conn->send(encoded);
        return;
    }

    // Queue through outbound queue
    PendingMessage msg{target, encoded, std::chrono::steady_clock::now()};
    outbound_queue_.try_enqueue(std::move(msg),
                                mailbox::DeliveryMode::BestEffort, TypeTag::User);
}

TransportSendResult
ConnectionPool::try_send(const ActorAddress& target, const StreamBuffer& encoded) {
    FAULT_INJECT("hpactor.connection_pool.try_send.fail") {
        return TransportSendResult::WriteError;
    }
    if (shutting_down_.load()) {
        return TransportSendResult::ShuttingDown;
    }

    if (!circuit_breaker_.allow_send()) {
        return TransportSendResult::CircuitOpen;
    }

    ConnectionPtr conn = get_connection();
    if (conn) {
        conn->send(encoded);
        return TransportSendResult::Sent;
    }

    PendingMessage msg{target, encoded, std::chrono::steady_clock::now()};
    auto result = outbound_queue_.try_enqueue(
        std::move(msg), mailbox::DeliveryMode::BestEffort, TypeTag::User);

    // Emit metric events for endpoint outbound queue send result
    if (metrics_ring_buffer_) {
        metrics::MetricEvent evt{};
        evt.actor_id = pack_endpoint_for_metrics();
        evt.code = 0;
        evt.value_hi = 1;
        evt.timestamp_ns = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());

        if (result == TransportSendResult::Sent) {
            evt.event_type = metrics::MetricEventType::kEndpointSendAccepted;
        } else {
            evt.event_type = metrics::MetricEventType::kEndpointSendRejected;
        }
        metrics_ring_buffer_->try_push(evt);
    }

    return result;
}

void ConnectionPool::send(const StreamBuffer& data) {
    // Create a minimal actor address using the remote endpoint
    ActorAddress target;
    target.endpoint =
        endpoint_ops::parse_endpoint(endpoint_ops::to_string(remote_endpoint_));
    (void)try_send(target, data);
}

void ConnectionPool::close() {
    abort();
}

bool ConnectionPool::is_connected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return !active_connections_.empty();
}

PoolStats ConnectionPool::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    PoolStats s;
    s.active_connections = active_connections_.size();
    s.pending_messages = outbound_queue_.total_messages();
    s.pending_control_messages = outbound_queue_.control_messages();
    s.pending_data_messages = outbound_queue_.data_messages();
    s.pending_bytes = outbound_queue_.total_bytes();
    s.reconnect_attempts = reconnect_attempts_.load();
    s.is_connected = !active_connections_.empty();
    s.pressure_state = static_cast<uint8_t>(outbound_queue_.pressure_state());
    s.circuit_state = static_cast<uint8_t>(circuit_breaker_.state());
    return s;
}

size_t ConnectionPool::drain() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t unsent = outbound_queue_.total_messages();
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
    return unsent;
}

void ConnectionPool::abort() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
}

void ConnectionPool::set_rpc_handler(rpc_response_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    rpc_handler_ = std::move(handler);
}

void ConnectionPool::set_spawn_handler(spawn_response_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    spawn_handler_ = std::move(handler);
}

void ConnectionPool::on_connection_ready(ConnectionPtr conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.push_back(conn);
    }
    circuit_breaker_.record_success();
    flush_pending();
}

void ConnectionPool::on_connection_error(ConnectionPtr conn, const error& err) {
    (void)err;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.erase(std::remove(active_connections_.begin(),
                                              active_connections_.end(), conn),
                                  active_connections_.end());
    }
    circuit_breaker_.record_failure();
    HPACTOR_LOG_ERROR(log::LogCategory::kNetwork, ActorId{0}, 0, "connection error");
    schedule_reconnect();
}

void ConnectionPool::on_frame_received(StreamBuffer frame_data) {
    FAULT_INJECT("hpactor.connection_pool.frame.drop") {
        return;
    }
    FAULT_INJECT("hpactor.transport.recv.drop") {
        return;
    }
    FAULT_INJECT("hpactor.transport.recv.corrupt") {
        if (frame_data.size() > 0) {
            frame_data.data()[0] ^= 0xFF;
        }
    }
    WireFrame frame = WireFrame::decode(frame_data);

    // Check for RPC response
    if (frame.pb_frame.flags() & WireFrame::RpcResponse) {
        // Try to decode as spawn response first
        if (static_cast<TypeTag>(frame.pb_frame.type_tag()) ==
            TypeTag::SpawnResponseTag) {
            ::hpactor::SpawnResponseMessage pb_resp;
            if (pb_resp.ParseFromArray(
                    frame.pb_frame.payload().data(),
                    static_cast<int>(frame.pb_frame.payload().size()))) {
                if (spawn_handler_) {
                    SpawnResponse resp;
                    resp.actor_addr = net::from_proto(pb_resp.actor_addr());
                    resp.error_code = pb_resp.error_code();
                    spawn_handler_(frame.pb_frame.message_id(), resp);
                    return;
                }
            }
        }

        // Fall through to RPC handler
        if (rpc_handler_) {
            RpcResponseFrame response;
            response.msg_id = MessageId(frame.pb_frame.message_id());
            response.payload = StreamBuffer(frame.pb_frame.payload().begin(),
                                            frame.pb_frame.payload().end());
            if (frame.pb_frame.has_trace_context()) {
                auto parsed = net::trace_context_from_proto(
                    frame.pb_frame.trace_context(), 256);
                if (parsed.has_value()) {
                    response.has_trace_context = true;
                    response.trace_context = parsed.value();
                }
            }
            rpc_handler_(response);
        }
        return;
    }

    // Route to actor message handler (deliver_remote)
    if (actor_message_handler_) {
        actor_message_handler_(frame);
    }
}

void ConnectionPool::schedule_reconnect() {
    FAULT_INJECT("hpactor.connection_pool.reconnect.drop") {
        return;
    }
    if (shutting_down_.load()) {
        return;
    }
    if (reconnect_attempts_.load() >= config_.max_attempts) {
        return; // Exhausted retries
    }
    if (reconnect_scheduled_.load()) {
        return;
    }
    reconnect_scheduled_.store(true);

    auto backoff = config_.initial_backoff;
    auto attempts = reconnect_attempts_.load();
    for (size_t i = 0; i < attempts; ++i) {
        backoff = backoff * 2;
        if (backoff > config_.max_backoff) {
            backoff = config_.max_backoff;
        }
    }

    reconnect_attempts_.fetch_add(1);
    loop_->run_after([this]() { reconnect_scheduled_.store(false); },
                     static_cast<int>(backoff.count()));
}

void ConnectionPool::flush_pending() {
    FAULT_INJECT("hpactor.connection_pool.flush.drop") {
        return;
    }
    while (true) {
        auto msg = outbound_queue_.try_dequeue();
        if (!msg.has_value())
            break;

        auto conn = get_connection();
        if (conn) {
            conn->send(msg->data);
        } else {
            break; // No connection available, stop draining
        }
    }
}

void ConnectionPool::add_connection(ConnectionPtr conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_connections_.push_back(conn);
}

void ConnectionPool::prewarm_pool(EndPoint ep,
                                  const std::vector<AcceptorInfo>& acceptors) {
    (void)ep; // Pool already bound to remote_endpoint_ from constructor
    std::lock_guard<std::mutex> lock(mutex_);
    acceptors_ = acceptors;
    // The actual connection is established asynchronously on first use.
    // This just ensures the pool is ready so the first send() doesn't pay
    // discovery cost.
}

ActorId ConnectionPool::pack_endpoint_for_metrics() const {
    // Pack the endpoint into a uint64_t for metric event transport.
    // For IPv4: upper 32 bits = address, lower 16 bits = port (network byte
    // order). For IPv6: fall back to a hash-based identifier.
    if (auto* ipv4 = std::get_if<Ipv4Endpoint>(&remote_endpoint_)) {
        uint64_t packed = (static_cast<uint64_t>(ipv4->addr) << 16) | ipv4->port_nw;
        return ActorId(packed);
    }
    // For IPv6, combine address bytes with port via FNV-1a hash.
    if (auto* ipv6 = std::get_if<Ipv6Endpoint>(&remote_endpoint_)) {
        uint64_t hash = 0xcbf29ce484222325ULL; // FNV-1a offset basis
        for (uint8_t byte : ipv6->addr) {
            hash ^= byte;
            hash *= 0x100000001b3ULL; // FNV-1a prime
        }
        hash ^= ipv6->port_nw;
        hash *= 0x100000001b3ULL;
        return ActorId(hash);
    }
    return ActorId(0);
}

} // namespace net
} // namespace hpactor
