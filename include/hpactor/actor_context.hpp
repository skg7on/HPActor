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

#include <hpactor/actor/abstract_actor.hpp>
#include <hpactor/actor/typed_message.hpp>
#include <hpactor/core/actor_ref_cache.hpp>
#include <hpactor/mailbox/mailbox_policy.hpp>
#include <hpactor/net/http_types.hpp>
#include <hpactor/ref/actor_address.hpp>
#include <hpactor/ref/actor_ref.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace google {
namespace protobuf {
class Message;
} // namespace protobuf
} // namespace google

namespace hpactor {

// -----------------------------------------------------------------------------
// ActorContext - execution context for actors
// -----------------------------------------------------------------------------
class ActorContext {
  public:
    explicit ActorContext(Actor owner, ActorSystem* system = nullptr);
    ~ActorContext();

    // Set the system reference (used when owner is not set)
    void set_system(ActorSystem* system) {
        system_ = system;
    }

    // Spawn child actors
    template <typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    template <typename T, typename... Args> T spawn(Args&&... args);

    // Send a pre-constructed TypedMessage
    void send(const ActorAddress& target, TypedMessage msg);

    // Primary: send to an already-resolved ActorRef (local or remote)
    void send(ActorRef& target, TypedMessage msg);

    // Send a protobuf message (serializes eagerly)
    void send(const ActorAddress& target, TypeTag tag,
              const google::protobuf::Message& msg);

    // Convenience: send a typed protobuf message
    template <typename ProtoMsgT>
    void send(const ActorAddress& target, const ProtoMsgT& msg);

    // Send with priority and deadline
    void send_with_priority(const ActorAddress& target, TypedMessage msg,
                            uint8_t priority, int64_t deadline_ns);

    // Try-send returning an admission result (opt-in backpressure).
    // Resolves the target address, stamps the sender address, and delegates
    // to ActorRef::try_send(). Returns ActorNotFound if resolution fails.
    mailbox::EnqueueResult try_send(const ActorAddress& target, TypedMessage msg,
                                    mailbox::DeliveryOptions options = {});

    // Try-send with priority and deadline, returning an admission result.
    // For local targets, delegates directly to
    // ActorSystem::try_deliver_local() with the given priority/deadline.
    // For remote targets, delegates to ActorRef::try_send().
    mailbox::EnqueueResult
    try_send_with_priority(const ActorAddress& target, TypedMessage msg,
                           uint8_t priority, int64_t deadline_ns,
                           mailbox::DeliveryOptions options = {});

    // Replies
    void reply(TypedMessage msg);
    void reply(TypeTag tag, const google::protobuf::Message& msg);
    template <typename ProtoMsgT> void reply(const ProtoMsgT& msg);
    void reply_with_error(const error& err);

    // Get the sender of the current message (for reply routing)
    const ActorAddress& current_sender() const {
        return current_sender_;
    }
    void set_current_sender(const ActorAddress& sender) {
        current_sender_ = sender;
    }

    // Scheduled execution
    AlarmHandle schedule(std::chrono::milliseconds delay, TypedMessage msg);
    void cancel_schedule(AlarmHandle handle);

    // Children management
    std::vector<Actor> children() const;
    void add_child(Actor child);
    void remove_child(Actor child);

    // Remote child management
    void add_remote_child(ActorRef child);
    std::vector<ActorRef> remote_children() const;

    // Link management (used by AbstractActor)
    std::vector<ActorAddress> linked_actors() const;
    void add_linked(const ActorAddress& addr) {
        linked_.push_back(addr);
    }
    void remove_linked(const ActorAddress& addr) {
        auto it = std::find(linked_.begin(), linked_.end(), addr);
        if (it != linked_.end())
            linked_.erase(it);
    }

    // Monitoring
    void monitor(const ActorAddress& target);

    // Monitor management (used by AbstractActor)
    void add_monitored(const ActorAddress& addr) {
        monitored_.push_back(addr);
    }
    void remove_monitored(const ActorAddress& addr) {
        auto it = std::find(monitored_.begin(), monitored_.end(), addr);
        if (it != monitored_.end())
            monitored_.erase(it);
    }
    const std::vector<ActorAddress>& monitored_actors() const {
        return monitored_;
    }

    // Resolve an ActorAddress to an ActorRef (lazy + cached)
    ActorRef resolve(const ActorAddress& target);

    // RPC calls (for non-actor threads only)
    RpcFuture<StreamBuffer>
    rpc(const ActorAddress& target, const StreamBuffer& encoded_request,
        std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(5000));

    // HTTP egress — async HTTP calls to external services.
    // Delegates to ActorSystem's HttpClient. Returns a future for the response
    // body.
    RpcFuture<StreamBuffer>
    http_get(const std::string& url, std::vector<net::HttpHeader> headers = {});
    RpcFuture<StreamBuffer> http_post(const std::string& url, StreamBuffer body,
                                      std::vector<net::HttpHeader> headers = {});
    RpcFuture<StreamBuffer> http_put(const std::string& url, StreamBuffer body,
                                     std::vector<net::HttpHeader> headers = {});
    RpcFuture<StreamBuffer>
    http_delete(const std::string& url, std::vector<net::HttpHeader> headers = {});
    RpcFuture<StreamBuffer>
    http_request(net::HttpMethod method, const std::string& url,
                 std::vector<net::HttpHeader> headers = {}, StreamBuffer body = {});

    // Current trace context for send/reply propagation
    bool has_current_trace_context() const noexcept {
        return has_current_trace_context_;
    }

    const TraceContext& current_trace_context() const noexcept {
        return current_trace_context_;
    }

    class TraceScope {
      public:
        TraceScope(ActorContext* ctx, const TraceContext& next) noexcept;
        ~TraceScope();
        TraceScope(const TraceScope&) = delete;
        TraceScope& operator=(const TraceScope&) = delete;

      private:
        ActorContext* ctx_{nullptr};
        TraceContext previous_{};
        bool previous_valid_{false};
    };

    // Backpressure signal handling
    using BackpressureHandler =
        std::function<void(const mailbox::BackpressureSignal&)>;

    void on_backpressure(BackpressureHandler handler);
    void handle_backpressure(const mailbox::BackpressureSignal& signal);

    // ── Graceful actor stop ────────────────────────────────────────────────
    // Initiates drain per the target actor's DrainPolicy.
    // Returns immediately; the actor drains on its scheduler thread.
    void stop(ActorId target);

    // Synchronous stop — blocks until target reaches kStopped or timeout.
    // Returns error on timeout. Do not call from actor threads.
    result<void> stop_sync(ActorId target, std::chrono::milliseconds timeout);

  private:
    Actor owner_;
    ActorSystem* system_ = nullptr;
    std::vector<Actor> children_;
    std::vector<ActorRef> remote_children_;
    std::vector<ActorAddress> linked_;
    std::vector<ActorAddress> monitored_;

    void set_current_trace_context(const TraceContext& context) noexcept {
        current_trace_context_ = context;
        has_current_trace_context_ = context.valid();
    }

    void clear_current_trace_context() noexcept {
        current_trace_context_.clear();
        has_current_trace_context_ = false;
    }

    ActorRefCache ref_cache_;
    ActorAddress current_sender_;
    BackpressureHandler backpressure_handler_;
    TraceContext current_trace_context_;
    bool has_current_trace_context_{false};
};

} // namespace hpactor
