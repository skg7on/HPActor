// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include "stream_runtime.hpp"

#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_handle.hpp>
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/msg/type_tag.hpp>
#include <hpactor/net/inbound_frame_sink.hpp>

#include "runtime/messaging_runtime.hpp"

namespace hpactor {

StreamRuntime::StreamRuntime(Config config, StreamActorLifecyclePort actor_port,
                             MessagingRuntime& messaging) noexcept
    : config_(config), actor_port_(actor_port), messaging_(messaging) {}

// ── Stream frame handlers ──────────────────────────────────────────────────

StreamDispatchResult
StreamRuntime::on_open(const net::InboundFrameContext& ictx,
                       const net::StreamOpenFrame& open) noexcept {
    uint64_t sid = open.stream_id();
    if (sid == 0) {
        return {StreamDispatchCode::InvalidFrame, 0, 0};
    }

    StreamKey key{ictx.peer, sid};

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Capacity check (Opening reservations count toward limit)
        if (sessions_.size() >= config_.max_active_streams) {
            return {StreamDispatchCode::CapacityExceeded, 0, 0};
        }

        // Duplicate check
        if (sessions_.find(key) != sessions_.end()) {
            return {StreamDispatchCode::DuplicateStream, 0, 0};
        }

        // Reserve Opening placeholder
        StreamSession session;
        session.state = StreamSessionState::Opening;
        session.generation = 1;
        sessions_[key] = session;
    }

    // Spawn receiver actor outside the lock
    ActorId target = net::from_proto(open.receiver()).id;
    ActorAddress sender_addr = net::from_proto(open.sender());
    TraceContext trace_ctx; // parse from open.trace_context() if present

    if (actor_port_.spawn_receiver) {
        auto spawned =
            actor_port_.spawn_receiver(actor_port_.context, target, sid, sender_addr,
                                       open.initial_window_bytes(), trace_ctx);

        if (!spawned.has_value()) {
            // Rollback: erase the Opening reservation
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = sessions_.find(key);
            if (it != sessions_.end() &&
                it->second.state == StreamSessionState::Opening &&
                it->second.generation == 1) {
                sessions_.erase(it);
            }
            return {StreamDispatchCode::SpawnFailed, 0, 0};
        }

        // Commit: transition Opening → Active
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(key);
        if (it != sessions_.end() && it->second.state == StreamSessionState::Opening &&
            it->second.generation == 1) {
            it->second.state = StreamSessionState::Active;
            it->second.receiver_actor = spawned.value();
            it->second.target_actor = target;
        }
    }

    return {StreamDispatchCode::Handled, 0, 0};
}

StreamDispatchResult
StreamRuntime::on_data(const net::InboundFrameContext& ictx,
                       const net::StreamDataFrame& data) noexcept {
    uint64_t sid = data.stream_id();
    StreamKey key{ictx.peer, sid};
    ActorId receiver;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(key);
        if (it == sessions_.end()) {
            return {StreamDispatchCode::UnknownStream, 0, 0};
        }
        receiver = it->second.receiver_actor;
    }

    if (receiver == ActorId{0}) {
        return {StreamDispatchCode::UnknownStream, 0, 0};
    }

    // Build TypedMessage with protobuf-aware constructor
    TypedMessage msg(stream::StreamDataTag, data);
    auto result = messaging_.try_deliver_fast(receiver, std::move(msg),
                                              FastDeliveryReason::StreamProtocol);

    if (result.accepted()) {
        return {StreamDispatchCode::Handled, 1, 0};
    }
    return {StreamDispatchCode::DeliveryRejected, 0, 1};
}

StreamDispatchResult StreamRuntime::on_ack(const net::InboundFrameContext& ictx,
                                           const net::StreamAckFrame& ack) noexcept {
    uint64_t sid = ack.stream_id();
    StreamKey key{ictx.peer, sid};
    ActorId sender;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(key);
        if (it == sessions_.end()) {
            return {StreamDispatchCode::UnknownStream, 0, 0};
        }
        sender = it->second.sender_actor;
    }

    if (sender == ActorId{0}) {
        return {StreamDispatchCode::UnknownStream, 0, 0};
    }

    TypedMessage msg(stream::StreamAckTag, ack);
    auto result = messaging_.try_deliver_fast(sender, std::move(msg),
                                              FastDeliveryReason::StreamProtocol);

    if (result.accepted()) {
        return {StreamDispatchCode::Handled, 1, 0};
    }
    return {StreamDispatchCode::DeliveryRejected, 0, 1};
}

StreamDispatchResult
StreamRuntime::on_close(const net::InboundFrameContext& ictx,
                        const net::StreamCloseFrame& close) noexcept {
    uint64_t sid = close.stream_id();
    StreamKey key{ictx.peer, sid};
    ActorId sender;
    ActorId receiver;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(key);
        if (it == sessions_.end()) {
            return {StreamDispatchCode::UnknownStream, 0, 0};
        }
        sender = it->second.sender_actor;
        receiver = it->second.receiver_actor;
        sessions_.erase(it); // atomically mark/erase
    }

    uint32_t delivered = 0;
    uint32_t rejected = 0;

    // Deliver close to both protocol actors using correct wire tags
    if (sender != ActorId{0}) {
        TypedMessage msg(stream::StreamCloseTag, close);
        auto r = messaging_.try_deliver_fast(sender, std::move(msg),
                                             FastDeliveryReason::StreamProtocol);
        if (r.accepted())
            ++delivered;
        else
            ++rejected;
    }
    if (receiver != ActorId{0}) {
        TypedMessage msg(stream::StreamCloseTag, close);
        auto r = messaging_.try_deliver_fast(receiver, std::move(msg),
                                             FastDeliveryReason::StreamProtocol);
        if (r.accepted())
            ++delivered;
        else
            ++rejected;
    }

    return {StreamDispatchCode::Handled, delivered, rejected};
}

StreamDispatchResult
StreamRuntime::on_error(const net::InboundFrameContext& ictx,
                        const net::StreamErrorFrame& error) noexcept {
    uint64_t sid = error.stream_id();
    StreamKey key{ictx.peer, sid};
    ActorId sender;
    ActorId receiver;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = sessions_.find(key);
        if (it == sessions_.end()) {
            return {StreamDispatchCode::UnknownStream, 0, 0};
        }
        sender = it->second.sender_actor;
        receiver = it->second.receiver_actor;
        sessions_.erase(it);
    }

    uint32_t delivered = 0;
    uint32_t rejected = 0;

    // Deliver error to both protocol actors using StreamWireErrorTag
    if (sender != ActorId{0}) {
        TypedMessage msg(stream::StreamWireErrorTag, error);
        auto r = messaging_.try_deliver_fast(sender, std::move(msg),
                                             FastDeliveryReason::StreamProtocol);
        if (r.accepted())
            ++delivered;
        else
            ++rejected;
    }
    if (receiver != ActorId{0}) {
        TypedMessage msg(stream::StreamWireErrorTag, error);
        auto r = messaging_.try_deliver_fast(receiver, std::move(msg),
                                             FastDeliveryReason::StreamProtocol);
        if (r.accepted())
            ++delivered;
        else
            ++rejected;
    }

    return {StreamDispatchCode::Handled, delivered, rejected};
}

// ── Facade API ─────────────────────────────────────────────────────────────

void StreamRuntime::register_sender(uint64_t stream_id, ActorId actor_id) {
    // Compatibility: local-only stream key (peer = local endpoint).
    // In Phase 5, peer is plumbed from NetworkRuntime.
    std::lock_guard<std::mutex> lock(mutex_);
    StreamKey key{endpoint_ops::parse_endpoint("127.0.0.1:0"), stream_id};
    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        it->second.sender_actor = actor_id;
    } else {
        StreamSession session;
        session.state = StreamSessionState::Active;
        session.sender_actor = actor_id;
        sessions_[key] = session;
    }
}

void StreamRuntime::register_receiver(uint64_t stream_id, ActorId actor_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamKey key{endpoint_ops::parse_endpoint("127.0.0.1:0"), stream_id};
    auto it = sessions_.find(key);
    if (it != sessions_.end()) {
        it->second.receiver_actor = actor_id;
    } else {
        StreamSession session;
        session.state = StreamSessionState::Active;
        session.receiver_actor = actor_id;
        sessions_[key] = session;
    }
}

void StreamRuntime::unregister_stream(uint64_t stream_id) {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamKey key{endpoint_ops::parse_endpoint("127.0.0.1:0"), stream_id};
    sessions_.erase(key);
}

uint64_t StreamRuntime::allocate_stream_id(ActorId sender_id) {
    uint64_t seq = counter_.fetch_add(1, std::memory_order_relaxed);
    return (static_cast<uint64_t>(sender_id.value()) << 32) | seq;
}

std::optional<StreamHandle>
StreamRuntime::open_stream(ActorId target, const StreamConfig& config) {
    (void)target;
    (void)config;
    return std::nullopt;
}

StreamRuntimeSnapshot StreamRuntime::snapshot() const {
    StreamRuntimeSnapshot snap{};
    snap.max_active = config_.max_active_streams;

    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [key, session] : sessions_) {
        StreamRuntimeSnapshot::Record rec;
        rec.key = key;
        rec.state = session.state;
        rec.sender_actor = session.sender_actor;
        rec.receiver_actor = session.receiver_actor;
        rec.target_actor = session.target_actor;
        snap.sessions.push_back(rec);
        if (session.state == StreamSessionState::Active)
            ++snap.active_count;
        if (session.state == StreamSessionState::Opening)
            ++snap.opening_count;
    }
    return snap;
}

} // namespace hpactor
