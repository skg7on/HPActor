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
#include <hpactor/core/actor_ref_cache.hpp>
#include <hpactor/msg/enqueue_result.hpp>
#include <hpactor/msg/request_handle.hpp>
#include <hpactor/msg/request_timeout.hpp>
#include <hpactor/msg/typed_message.hpp>
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

/// \brief Execution context provided to every actor.
///
/// The primary API for message sending, child spawning, scheduling, RPC,
/// HTTP egress, lifecycle management, and linking/monitoring. Each actor
/// receives exactly one context, created by \c ActorSystem during spawn.
///
/// \note Thread safety: All methods must be called from the actor's own
///       thread or scheduler worker. \c rpc(), \c http_*, and \c stop_sync()
///       are safe from non-actor threads.
class ActorContext {
  public:
    /// \brief Construct a context for the given actor.
    /// \param[in] owner The owning actor.
    /// \param[in] system Pointer to the owning \c ActorSystem (may be null
    ///                   for the system pseudo-actor).
    explicit ActorContext(Actor owner, ActorSystem* system = nullptr);
    ~ActorContext();

    /// \brief Set the system back-reference (used when owner is unset).
    void set_system(ActorSystem* system) {
        system_ = system;
    }

    // ── Actor spawning ────────────────────────────────────────────────────

    /// \brief Spawn a child actor from a factory function.
    ///
    /// \tparam Fn Callable that returns an actor.
    /// \tparam Args Argument types forwarded to the factory.
    /// \param[in] fn Factory function.
    /// \param[in] args Arguments forwarded to the factory.
    /// \return An \c Actor handle to the spawned child.
    template <typename Fn, typename... Args>
    Actor spawn(Fn&& fn, Args&&... args);

    /// \brief Spawn a child actor by type.
    ///
    /// \tparam T Actor subclass to instantiate.
    /// \tparam Args Constructor argument types.
    /// \param[in] args Constructor arguments.
    /// \return An \c Actor handle to the spawned child.
    template <typename T, typename... Args> T spawn(Args&&... args);

    // ── Message sending ───────────────────────────────────────────────────

    /// \brief Send a pre-constructed \c TypedMessage to a target address.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] msg Message to send (moved).
    void send(const ActorAddress& target, TypedMessage msg);

    /// \brief Send to an already-resolved \c ActorRef (local or remote).
    ///
    /// \param[in,out] target Resolved actor reference.
    /// \param[in] msg Message to send (moved).
    void send(ActorRef& target, TypedMessage msg);

    /// \brief Send a protobuf message (serializes eagerly before enqueue).
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] tag Type tag that identifies the message type.
    /// \param[in] msg Protobuf message to serialize and send.
    void send(const ActorAddress& target, TypeTag tag,
              const google::protobuf::Message& msg);

    /// \brief Convenience overload — send a typed protobuf message.
    ///
    /// \tparam ProtoMsgT Protobuf message type (must have associated \c
    /// TypeTag).
    /// \param[in] target Destination actor address.
    /// \param[in] msg Protobuf message instance.
    template <typename ProtoMsgT>
    void send(const ActorAddress& target, const ProtoMsgT& msg);

    /// \brief Send with priority and deadline for scheduler ordering.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] msg Message to send.
    /// \param[in] priority 0–3 (0 = highest priority).
    /// \param[in] deadline_ns Absolute deadline in nanoseconds
    ///                       (\c INT64_MAX = no deadline).
    void send_with_priority(const ActorAddress& target, TypedMessage msg,
                            uint8_t priority, int64_t deadline_ns);

    /// \brief Try-send returning a unified delivery result.
    ///
    /// Resolves the target address, stamps the sender address, and delegates
    /// to \c ActorRef::try_send(). Returns a \c DeliveryResult suitable for
    /// user-facing logic (retry, backoff, error handling).
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] msg Message to send.
    /// \param[in] options Delivery options (deadline, priority, idempotency).
    /// \return \c DeliveryResult describing the delivery outcome.
    mailbox::DeliveryResult try_send(const ActorAddress& target, TypedMessage msg,
                                     mailbox::DeliveryOptions options = {});

    /// \brief Try-send with explicit priority and deadline.
    ///
    /// For local targets, delegates directly to
    /// \c ActorSystem::try_deliver_local(). For remote targets, delegates
    /// to \c ActorRef::try_send(). Maps results to \c DeliveryResult.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] msg Message to send.
    /// \param[in] priority 0–3 (0 = highest).
    /// \param[in] deadline_ns Absolute deadline in nanoseconds.
    /// \param[in] options Delivery options.
    /// \return \c DeliveryResult describing the delivery outcome.
    mailbox::DeliveryResult
    try_send_with_priority(const ActorAddress& target, TypedMessage msg,
                           uint8_t priority, int64_t deadline_ns,
                           mailbox::DeliveryOptions options = {});

    /// \brief Try-reply to the current sender, returning a delivery result.
    ///
    /// \param[in] msg Message to send back.
    /// \param[in] options Delivery options.
    /// \return \c DeliveryResult with NoRoute if there is no current sender.
    mailbox::DeliveryResult
    try_reply(TypedMessage msg, mailbox::DeliveryOptions options = {});

    // ── Replies ───────────────────────────────────────────────────────────

    /// \brief Reply to the sender of the current message.
    ///
    /// \param[in] msg Message to send back.
    void reply(TypedMessage msg);

    /// \brief Reply with a protobuf message.
    ///
    /// \param[in] tag Type tag for the response.
    /// \param[in] msg Protobuf message to serialize and send.
    void reply(TypeTag tag, const google::protobuf::Message& msg);

    /// \brief Convenience — reply with a typed protobuf message.
    ///
    /// \tparam ProtoMsgT Protobuf message type.
    /// \param[in] msg Protobuf message instance.
    template <typename ProtoMsgT> void reply(const ProtoMsgT& msg);

    /// \brief Reply with an error to the current sender.
    ///
    /// \param[in] err Error code and optional detail.
    void reply_with_error(const error& err);

    /// \brief Address of the sender of the current message.
    const ActorAddress& current_sender() const {
        return current_sender_;
    }
    /// \brief Set the current sender address (called by message dispatch).
    void set_current_sender(const ActorAddress& sender) {
        current_sender_ = sender;
    }

    /// \brief Current ask message ID for reply routing through AskManager.
    uint64_t current_ask_message_id() const {
        return current_ask_message_id_;
    }
    /// \brief Set the ask message ID (called by message dispatch).
    void set_current_ask_message_id(uint64_t id) {
        current_ask_message_id_ = id;
    }

    // ── Scheduled delivery ────────────────────────────────────────────────

    /// \brief Schedule self-delivery of a message after a delay.
    ///
    /// \param[in] delay Time until delivery.
    /// \param[in] msg Message to deliver to self.
    /// \return An \c AlarmHandle that can be used to cancel the schedule.
    AlarmHandle schedule(std::chrono::milliseconds delay, TypedMessage msg);

    /// \brief Cancel a previously scheduled message.
    ///
    /// \param[in] handle The handle returned by \c schedule().
    void cancel_schedule(AlarmHandle handle);

    /// \brief Request self-passivation after the current message completes.
    ///
    /// The passivation is deferred until the current handler returns,
    /// ensuring consistent state for snapshotting. If the actor is not
    /// in \c kActive state, this call is a no-op.
    ///
    /// \note Callable only from within an actor handler.
    void passivate();

    // ── Children management ───────────────────────────────────────────────

    /// \brief List of direct child actors.
    std::vector<Actor> children() const;
    /// \brief Register a direct child.
    void add_child(Actor child);
    /// \brief Unregister a direct child.
    void remove_child(Actor child);

    /// \brief Register a remote child actor.
    void add_remote_child(ActorRef child);
    /// \brief List of remote children (spawned on other nodes).
    std::vector<ActorRef> remote_children() const;

    // ── Link management ───────────────────────────────────────────────────

    /// \brief Addresses of linked actors (bidirectional death sharing).
    std::vector<ActorAddress> linked_actors() const;
    void add_linked(const ActorAddress& addr) {
        linked_.push_back(addr);
    }
    void remove_linked(const ActorAddress& addr) {
        auto it = std::find(linked_.begin(), linked_.end(), addr);
        if (it != linked_.end())
            linked_.erase(it);
    }

    // ── Monitoring ────────────────────────────────────────────────────────

    /// \brief Register one-way monitoring of \p target.
    ///
    /// When \p target terminates this actor receives a \c DownMsg.
    /// \param[in] target Actor to monitor.
    void monitor(const ActorAddress& target);

    void add_monitored(const ActorAddress& addr) {
        monitored_.push_back(addr);
    }
    void remove_monitored(const ActorAddress& addr) {
        auto it = std::find(monitored_.begin(), monitored_.end(), addr);
        if (it != monitored_.end())
            monitored_.erase(it);
    }
    /// \brief List of currently monitored actor addresses.
    const std::vector<ActorAddress>& monitored_actors() const {
        return monitored_;
    }

    // ── Actor resolution ──────────────────────────────────────────────────

    /// \brief Resolve an address to an \c ActorRef (lazy + cached).
    ///
    /// \param[in] target Actor address to resolve.
    /// \return A resolved \c ActorRef for local or remote dispatch.
    ActorRef resolve(const ActorAddress& target);

    // ── RPC ───────────────────────────────────────────────────────────────

    /// \brief Issue an asynchronous RPC call.
    ///
    /// Safe from non-actor threads.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] encoded_request Pre-serialized request payload.
    /// \param[in] timeout_ms Maximum time to wait for a response (default 5 s).
    /// \return An \c RpcFuture that yields the response body.
    /// \note Thread safety: Safe from any thread.
    RpcFuture<StreamBuffer>
    rpc(const ActorAddress& target, const StreamBuffer& encoded_request,
        std::chrono::milliseconds timeout_ms = std::chrono::milliseconds(5000));

    // ── Ask (request-response with timeout) ────────────────────────────────

    /// \brief Send a request and get a handle for the response.
    ///
    /// Routes locally for same-process targets or via RpcChannel for
    /// remote targets.
    ///
    /// \param[in] target Destination actor address.
    /// \param[in] encoded_request Pre-serialized request payload.
    /// \param[in] timeout Per-request timeout (default: system config).
    /// \return RequestHandle that resolves with the response or error.
    RequestHandle<StreamBuffer>
    ask_raw(const ActorAddress& target, const StreamBuffer& encoded_request,
            RequestTimeout timeout = RequestTimeout::use_default());

    // ── HTTP egress ───────────────────────────────────────────────────────

    /// \brief Async HTTP GET to an external service.
    ///
    /// \param[in] url Target URL.
    /// \param[in] headers Optional HTTP headers.
    /// \return An \c RpcFuture yielding the response body.
    /// \note Thread safety: Safe from any thread. Delegates to
    ///       \c ActorSystem's \c HttpClient.
    RpcFuture<StreamBuffer>
    http_get(const std::string& url, std::vector<net::HttpHeader> headers = {});

    /// \brief Async HTTP POST to an external service.
    ///
    /// \param[in] url Target URL.
    /// \param[in] body Request body.
    /// \param[in] headers Optional HTTP headers.
    /// \return An \c RpcFuture yielding the response body.
    RpcFuture<StreamBuffer> http_post(const std::string& url, StreamBuffer body,
                                      std::vector<net::HttpHeader> headers = {});

    /// \brief Async HTTP PUT to an external service.
    RpcFuture<StreamBuffer> http_put(const std::string& url, StreamBuffer body,
                                     std::vector<net::HttpHeader> headers = {});

    /// \brief Async HTTP DELETE to an external service.
    RpcFuture<StreamBuffer>
    http_delete(const std::string& url, std::vector<net::HttpHeader> headers = {});

    /// \brief Generic async HTTP request.
    ///
    /// \param[in] method HTTP method (GET, POST, PUT, DELETE).
    /// \param[in] url Target URL.
    /// \param[in] headers Optional HTTP headers.
    /// \param[in] body Request body (ignored for GET/DELETE).
    /// \return An \c RpcFuture yielding the response body.
    RpcFuture<StreamBuffer>
    http_request(net::HttpMethod method, const std::string& url,
                 std::vector<net::HttpHeader> headers = {}, StreamBuffer body = {});

    // ── Distributed tracing ───────────────────────────────────────────────

    /// \brief Returns \c true if a trace context is active for the current
    ///        message being processed.
    bool has_current_trace_context() const noexcept {
        return has_current_trace_context_;
    }

    /// \brief Active trace context for send/reply propagation.
    const TraceContext& current_trace_context() const noexcept {
        return current_trace_context_;
    }

    /// \brief RAII guard that pushes a trace context and restores the
    ///        previous one on destruction.
    ///
    /// Used by the receive path to set the incoming span as current.
    /// \note Thread safety: Scoped to a single actor thread.
    class TraceScope {
      public:
        /// \brief Push \p next as the current trace context.
        /// \param[in] ctx Owning \c ActorContext.
        /// \param[in] next Incoming trace context to activate.
        TraceScope(ActorContext* ctx, const TraceContext& next) noexcept;
        /// \brief Restore the previous trace context.
        ~TraceScope();
        TraceScope(const TraceScope&) = delete;
        TraceScope& operator=(const TraceScope&) = delete;

      private:
        ActorContext* ctx_{nullptr};
        TraceContext previous_{};
        bool previous_valid_{false};
    };

    // ── Backpressure ──────────────────────────────────────────────────────

    /// \brief Callback invoked when a downstream mailbox emits a backpressure
    ///        signal.
    using BackpressureHandler =
        std::function<void(const mailbox::BackpressureSignal&)>;

    /// \brief Register a backpressure handler.
    void on_backpressure(BackpressureHandler handler);

    /// \brief Deliver a backpressure signal from a downstream mailbox.
    ///
    /// Invokes the registered \c BackpressureHandler.
    /// \param[in] signal Backpressure signal from downstream.
    void handle_backpressure(const mailbox::BackpressureSignal& signal);

    // ── Graceful stop ─────────────────────────────────────────────────────

    /// \brief Initiate graceful drain of \p target per its \c DrainPolicy.
    ///
    /// Returns immediately; the target drains on its scheduler thread.
    /// \param[in] target Actor ID to stop.
    void stop(ActorId target);

    /// \brief Synchronous stop — blocks until \p target reaches \c kStopped
    ///        or \p timeout expires.
    ///
    /// \param[in] target Actor ID to stop.
    /// \param[in] timeout Maximum time to wait.
    /// \return \c result<void> with error on timeout.
    /// \note Thread safety: Safe from non-actor threads. Do not call from
    ///       actor scheduler threads.
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
    uint64_t current_ask_message_id_ = 0;
    BackpressureHandler backpressure_handler_;
    TraceContext current_trace_context_;
    bool has_current_trace_context_{false};
    bool passivation_requested_{false};
};

} // namespace hpactor
