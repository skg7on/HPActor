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

#include <hpactor/actor/external_msg_gateway.hpp>
#include <hpactor/mem/std_allocator.hpp>
#include <hpactor/net/http_gateway.hpp>
#include <hpactor/net/http_serializer.hpp>
#include <hpactor/net/http_types.hpp>

#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <queue>
#include <unordered_map>

namespace hpactor {
namespace net {

// ---------------------------------------------------------------------------
// HTTPGatewayActor — ExternalMsgGatewayActor-based HTTP ingress gateway
// ---------------------------------------------------------------------------
class HTTPGatewayActor : public ExternalMsgGatewayActor {
  public:
    using MessageBuilder = RouteRegistry::MessageBuilder;

    HTTPGatewayActor(ActorContext* ctx, ActorSystem& sys,
                     const std::string& bind_host, uint16_t port);
    ~HTTPGatewayActor() override;

    HTTPGatewayActor(const HTTPGatewayActor&) = delete;
    HTTPGatewayActor& operator=(const HTTPGatewayActor&) = delete;

    // Route registration (HTTP-method-aware)
    void route(HttpMethod method, std::string path_pattern,
               MessageBuilder builder, int priority = 0);
    void route(HttpMethod method, std::string path_pattern, ActorAddr target);

    // Configuration
    void set_reply_timeout(std::chrono::milliseconds t) { reply_timeout_ = t; }
    void set_max_connections(size_t max) { gateway_.set_max_connections(max); }
    void set_max_request_size(size_t max) { gateway_.set_max_request_size(max); }

    uint16_t port() const { return gateway_.port(); }
    bool is_listening() const { return gateway_.is_listening(); }

    // DaemonActor overrides
    bool run_once() override;

  protected:
    void on_daemon_start() override;
    void on_daemon_stop() override;
    void on_deactivate() override;

  private:
    void on_request(HTTPConnection* conn, HttpRequest&& req);
    void on_reply(TypedMessage&& msg);
    void on_error(HTTPConnection* conn, const error& err);
    void on_timeout(uint64_t request_id);
    void close_connection(HTTPConnection* conn);

    HTTPGateway gateway_;
    RouteRegistry routes_;
    std::unique_ptr<HttpSerializer> serializer_;

    struct PendingReply : mem::SlabAllocated<PendingReply> {
        uint64_t request_id;
        HTTPConnection* conn;
        std::chrono::steady_clock::time_point enqueued_at;
    };

    using PendingReplyMap =
        std::unordered_map<uint64_t, std::unique_ptr<PendingReply>,
                           std::hash<uint64_t>, std::equal_to<>,
                           mem::MemStdAllocator<std::pair<const uint64_t,
                                                          std::unique_ptr<PendingReply>>>>;
    PendingReplyMap pending_replies_{
        mem::MemStdAllocator<std::pair<const uint64_t,
                                       std::unique_ptr<PendingReply>>>(
            id_ptr(), mem::RegionType::kActor)};
    std::mutex reply_mutex_;

    using ReplyDeque =
        std::deque<TypedMessage, mem::MemStdAllocator<TypedMessage>>;
    std::queue<TypedMessage, ReplyDeque> reply_queue_{
        ReplyDeque(mem::MemStdAllocator<TypedMessage>(
            id_ptr(), mem::RegionType::kActor))};
    std::mutex reply_queue_mutex_;

    Actor reply_adapter_{nullptr};

    std::chrono::milliseconds reply_timeout_{5000};
    size_t max_connections_{1000};
    size_t max_request_size_{1048576};
    uint64_t next_request_id_{1};
};

} // namespace net
} // namespace hpactor
