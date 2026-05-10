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

#include <hpactor/actor/http_gateway_actor.hpp>
#include <hpactor/core/actor_system.hpp>

#include <cstring>

namespace hpactor {
namespace net {

// =============================================================================
// ReplyAdapter — internal EventBasedActor for receiving actor replies
// =============================================================================
namespace {

class ReplyAdapter final : public EventBasedActor {
  public:
    using ReplyHandler = std::function<void(TypedMessage&&)>;

    ReplyAdapter(ActorContext* ctx, ActorSystem& sys, ReplyHandler handler)
        : EventBasedActor(ctx, sys), handler_(std::move(handler)) {
        become(make_behavior());
    }

    Behavior make_behavior() override {
        return Behavior([this](TypedMessage& msg) {
            if (handler_) {
                handler_(std::move(msg));
            }
        });
    }

  private:
    ReplyHandler handler_;
};

} // anonymous namespace

// =============================================================================
// HTTPGatewayActor Implementation
// =============================================================================

HTTPGatewayActor::HTTPGatewayActor(ActorContext* ctx, ActorSystem& sys,
                                   const std::string& bind_host, uint16_t port)
    : ExternalMsgGatewayActor(ctx, sys),
      serializer_(std::make_unique<HttpSerializer>()) {
    auto handler = [this](TypedMessage&& msg) {
        std::lock_guard<std::mutex> lock(reply_queue_mutex_);
        reply_queue_.push(std::move(msg));
    };
    reply_adapter_ = system().spawn<ReplyAdapter>(std::move(handler));

    gateway_.set_request_handler([this](HTTPConnection* conn, HttpRequest&& req) {
        on_request(conn, std::move(req));
    });
    gateway_.set_error_handler([this](HTTPConnection* conn, const error& err) {
        on_error(conn, err);
    });
    gateway_.set_max_connections(max_connections_);
    gateway_.set_max_request_size(max_request_size_);

    gateway_.listen(port, bind_host);
}

HTTPGatewayActor::~HTTPGatewayActor() {
    on_deactivate();
}

void HTTPGatewayActor::route(HttpMethod method, std::string path_pattern,
                             MessageBuilder builder, int priority) {
    routes_.add(method, std::move(path_pattern), std::move(builder), priority);
}

void HTTPGatewayActor::route(HttpMethod method, std::string path_pattern,
                             ActorAddr target) {
    auto serializer = serializer_.get();
    route(method, std::move(path_pattern),
          [target, serializer](
              const HttpRequest& req) -> std::pair<ActorAddress, TypedMessage> {
              auto result = serializer->deserialize_request(req, TypeTag::User);
              if (!result.has_value()) {
                  return {invalid_actor_addr, TypedMessage{}};
              }
              return {target, std::move(result.value())};
          });
}

void HTTPGatewayActor::on_daemon_start() {}

void HTTPGatewayActor::on_daemon_stop() {
    gateway_.stop();

    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        pending_replies_.clear();
    }
}

bool HTTPGatewayActor::run_once() {
    gateway_.run_once();

    for (;;) {
        TypedMessage msg;
        {
            std::lock_guard<std::mutex> lock(reply_queue_mutex_);
            if (reply_queue_.empty())
                break;
            msg = std::move(reply_queue_.front());
            reply_queue_.pop();
        }
        on_reply(std::move(msg));
    }

    return true;
}

void HTTPGatewayActor::on_deactivate() {
    gateway_.stop();
    ExternalMsgGatewayActor::on_deactivate();
}

void HTTPGatewayActor::on_request(HTTPConnection* conn,
                                  HttpRequest&& req) { // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
    size_t qpos = req.path.find('?');
    if (qpos != std::string::npos) {
        std::string query_str = req.path.substr(qpos + 1);
        req.path = req.path.substr(0, qpos);
        size_t pos = 0;
        while (pos < query_str.size()) {
            size_t eq = query_str.find('=', pos);
            size_t amp = query_str.find('&', pos);
            if (eq != std::string::npos && (amp == std::string::npos || eq < amp)) {
                size_t vstart = eq + 1;
                size_t vend = amp == std::string::npos ? query_str.size() : amp;
                req.query_params[query_str.substr(pos, eq - pos)] =
                    query_str.substr(vstart, vend - vstart);
            }
            pos = amp == std::string::npos ? query_str.size() : amp + 1;
        }
    }

    const auto* builder = routes_.match(req.method, req.path, req);
    if (!builder) {
        StreamBuffer body;
        gateway_.send_response(
            conn, HttpStatusCode::NotFound,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            std::move(body));
        close_connection(conn);
        return;
    }

    auto [target, msg] = (*builder)(req);
    if (!target) {
        StreamBuffer body;
        gateway_.send_response(
            conn, HttpStatusCode::InternalError,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            std::move(body));
        close_connection(conn);
        return;
    }

    uint64_t request_id = next_request_id_++;
    auto pending = std::make_unique<PendingReply>();
    pending->request_id = request_id;
    pending->conn = conn;
    pending->enqueued_at = std::chrono::steady_clock::now();
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        pending_replies_[request_id] = std::move(pending);
    }

    StreamBuffer correlated;
    for (int i = 7; i >= 0; --i) {
        correlated.push_back(static_cast<uint8_t>((request_id >> (i * 8)) & 0xFF));
    }
    correlated.append(msg.payload().data(), msg.payload().size());

    TypedMessage correlated_msg(msg.type_id(), correlated);
    correlated_msg.set_sender_address(reply_adapter_.address());

    auto result = system().try_deliver_local(target.id, std::move(correlated_msg));
    if (!result.accepted()) {
        // Clean up the pending reply entry we just created
        {
            std::lock_guard<std::mutex> lock(reply_mutex_);
            auto it = pending_replies_.find(request_id);
            if (it != pending_replies_.end()) {
                // Cancel the timeout timer for this request
                it->second->conn = nullptr;
                pending_replies_.erase(it);
            }
        }

        StreamBuffer body;
        const char* msg_str = "Too Many Requests";
        body.append(reinterpret_cast<const uint8_t*>(msg_str), strlen(msg_str));
        gateway_.send_response(conn, HttpStatusCode::TooManyRequests,
                               {{"Content-Type", "text/plain"},
                                {"Connection", "close"},
                                {"Retry-After", "5"}},
                               std::move(body));
        close_connection(conn);
        return;
    }

    int timeout_ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(reply_timeout_).count());
    gateway_.event_loop().run_after(
        [this, request_id] { on_timeout(request_id); }, timeout_ms);
}

void HTTPGatewayActor::on_reply(TypedMessage&& msg) { // NOLINT(cppcoreguidelines-rvalue-reference-param-not-moved)
    const auto& payload = msg.payload();
    if (payload.size() < 8)
        return;

    uint64_t request_id = 0;
    for (int i = 0; i < 8; ++i) {
        request_id = (request_id << 8) | payload.data()[i];
    }

    HTTPConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        auto it = pending_replies_.find(request_id);
        if (it == pending_replies_.end())
            return;
        conn = it->second->conn;
        pending_replies_.erase(it);
    }

    if (!conn)
        return;

    StreamBuffer reply_payload;
    reply_payload.append(payload.data() + 8, payload.size() - 8);
    TypedMessage reply_msg(msg.type_id(), reply_payload);

    auto [body, content_type] = serializer_->serialize_response(reply_msg, "app"
                                                                           "lic"
                                                                           "ati"
                                                                           "on/"
                                                                           "jso"
                                                                           "n");

    std::vector<HttpHeader> headers = {{"Content-Type", content_type}};

    if (!conn->should_keep_alive()) {
        headers.push_back({"Connection", "close"});
    }

    gateway_.send_response(conn, HttpStatusCode::OK, std::move(headers),
                           std::move(body));

    if (!conn->should_keep_alive()) {
        close_connection(conn);
    }
}

void HTTPGatewayActor::on_error(HTTPConnection* conn, const error& err) {
    HttpStatusCode code = HttpStatusCode::InternalError;
    if (err.code() == errors::http_parse_error) {
        code = HttpStatusCode::BadRequest;
    }

    StreamBuffer body;
    const auto& msg = err.message();
    if (!msg.empty()) {
        body.append(reinterpret_cast<const uint8_t*>(msg.data()), msg.size());
    }

    gateway_.send_response(
        conn, code, {{"Content-Type", "text/plain"}, {"Connection", "close"}},
        std::move(body));
    close_connection(conn);
}

void HTTPGatewayActor::on_timeout(uint64_t request_id) {
    HTTPConnection* conn = nullptr;
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        auto it = pending_replies_.find(request_id);
        if (it == pending_replies_.end())
            return;
        conn = it->second->conn;
        pending_replies_.erase(it);
    }

    if (conn) {
        StreamBuffer body;
        const char* msg_str = "Upstream actor did not respond in time";
        body.append(reinterpret_cast<const uint8_t*>(msg_str), strlen(msg_str));
        gateway_.send_response(
            conn, HttpStatusCode::GatewayTimeout,
            {{"Content-Type", "text/plain"}, {"Connection", "close"}},
            std::move(body));
        close_connection(conn);
    }
}

void HTTPGatewayActor::close_connection(HTTPConnection* conn) {
    {
        std::lock_guard<std::mutex> lock(reply_mutex_);
        for (auto it = pending_replies_.begin(); it != pending_replies_.end();) {
            if (it->second->conn == conn) {
                it = pending_replies_.erase(it);
            } else {
                ++it;
            }
        }
    }

    gateway_.close_connection(conn);
}

} // namespace net
} // namespace hpactor
