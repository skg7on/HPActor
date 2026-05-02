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

#include <hpactor/net/connection_pool.hpp>
#include <hpactor/net/frame.hpp>
#include <hpactor/spawn.hpp>

#include <hpactor/common.pb.h>
#include <hpactor/messages.pb.h>

namespace hpactor {

namespace net {

ConnectionPool::ConnectionPool(EndPoint remote_endpoint,
                               const PoolConfig& config,
                               TlsContext* tls_context, EventLoop* loop)
    : remote_endpoint_(remote_endpoint), config_(config),
      tls_context_(tls_context), loop_(loop) {}

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
    if (shutting_down_.load()) {
        return;
    }

    ConnectionPtr conn = get_connection();
    if (conn) {
        conn->send(encoded);
        return;
    }

    // No connection available, queue pending
    if (!add_pending(target, encoded)) {
        return; // Queue full
    }

    // Create connection
    create_connection();
}

void ConnectionPool::send(const StreamBuffer& data) {
    // Create a minimal actor address using the remote endpoint
    ActorAddress target;
    target.endpoint =
        endpoint_ops::parse_endpoint(endpoint_ops::to_string(remote_endpoint_));
    send(target, data);
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
    s.pending_messages = pending_messages_.size();
    s.reconnect_attempts = reconnect_attempts_.load();
    s.is_connected = !active_connections_.empty();
    return s;
}

size_t ConnectionPool::drain() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    size_t unsent = pending_messages_.size();
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
    pending_messages_.clear();
    return unsent;
}

void ConnectionPool::abort() {
    shutting_down_.store(true);
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& conn : active_connections_) {
        conn->close();
    }
    active_connections_.clear();
    pending_messages_.clear();
}

void ConnectionPool::set_rpc_handler(rpc_response_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    rpc_handler_ = std::move(handler);
}

void ConnectionPool::set_spawn_handler(spawn_response_handler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    spawn_handler_ = std::move(handler);
}

ConnectionPtr ConnectionPool::create_connection() {
    if (connecting_.load()) {
        return nullptr;
    }
    if (!connecting_.exchange(true)) {
        // Determine local endpoint from loop (default to LocalEndpoint)
        EndPoint local_ep = LocalEndpoint;

        ConnectionPtr conn;
        if (config_.use_tls) {
            auto tls_conn =
                TlsConnection::create_client(local_ep, remote_endpoint_,
                                             tls_context_, loop_);
            tls_conn->set_ready_handler(
                [this](ConnectionPtr c) { on_connection_ready(c); });
            tls_conn->set_error_handler([this](ConnectionPtr c, const error& e) {
                on_connection_error(c, e);
            });
            tls_conn->set_frame_handler(
                [this](StreamBuffer data) { on_frame_received(std::move(data)); });
            conn = tls_conn;
        } else {
            // WireFrame connection for internal create_connection — fd will be set
            // after TCP connect. Create with fd=-1 for now.
            auto plain_conn =
                WireFrameConnection::create_as_client(-1, local_ep, remote_endpoint_, loop_);
            plain_conn->set_ready_handler(
                [this](ConnectionPtr c) { on_connection_ready(c); });
            plain_conn->set_error_handler([this](ConnectionPtr c, const error& e) {
                on_connection_error(c, e);
            });
            plain_conn->set_frame_handler(
                [this](StreamBuffer data) { on_frame_received(std::move(data)); });
            conn = plain_conn;
        }
        return conn;
    }
    return nullptr;
}

void ConnectionPool::on_connection_ready(ConnectionPtr conn) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        active_connections_.push_back(conn);
    }
    connecting_.store(false);
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
    schedule_reconnect();
}

void ConnectionPool::on_frame_received(StreamBuffer frame_data) {
    WireFrame frame = WireFrame::decode(frame_data);

    // Check for RPC response
    if (frame.pb_frame.flags() & WireFrame::RpcResponse) {
        // Try to decode as spawn response first
        if (static_cast<TypeTag>(frame.pb_frame.type_tag()) == TypeTag::SpawnResponseTag) {
            ::hpactor::SpawnResponseMessage pb_resp;
            if (pb_resp.ParseFromArray(frame.pb_frame.payload().data(),
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
            rpc_handler_(MessageId(frame.pb_frame.message_id()),
                         StreamBuffer(frame.pb_frame.payload().begin(),
                                      frame.pb_frame.payload().end()));
        }
        return;
    }

    // Route to actor message handler (deliver_remote)
    if (actor_message_handler_) {
        actor_message_handler_(frame);
    }
}

void ConnectionPool::schedule_reconnect() {
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
    loop_->run_after(
        [this]() {
            reconnect_scheduled_.store(false);
            connecting_.store(false);
            if (!shutting_down_.load()) {
                create_connection();
            }
        },
        static_cast<int>(backoff.count()));
}

void ConnectionPool::flush_pending() {
    std::lock_guard<std::mutex> lock(mutex_);
    while (!pending_messages_.empty() && !active_connections_.empty()) {
        auto& msg = pending_messages_.front();
        auto conn = get_connection();
        if (conn) {
            conn->send(msg.data);
            pending_messages_.pop_front();
        } else {
            break;
        }
    }
}

bool ConnectionPool::add_pending(const ActorAddress& target, const StreamBuffer& data) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_messages_.size() >= config_.max_pending) {
        return false;
    }
    pending_messages_.push_back({target, data, std::chrono::steady_clock::now()});
    return true;
}

void ConnectionPool::add_connection(ConnectionPtr conn) {
    std::lock_guard<std::mutex> lock(mutex_);
    active_connections_.push_back(conn);
}

} // namespace net
} // namespace hpactor
