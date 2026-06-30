// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)

#include "stream_runtime.hpp"

#include <hpactor/actor/stream_config.hpp>
#include <hpactor/actor/stream_handle.hpp>
#include <hpactor/actor/stream_types.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/inbound_frame_sink.hpp>

namespace hpactor {

StreamRuntime::StreamRuntime(Config config, StreamActorLifecyclePort actor_port,
                             MessagingRuntime& messaging) noexcept
    : config_(config), actor_port_(actor_port), messaging_(messaging) {}

// ── Stream frame handlers ──────────────────────────────────────────────────

StreamDispatchResult
StreamRuntime::on_open(const net::InboundFrameContext& /*ictx*/,
                       const net::StreamOpenFrame& /*open*/) noexcept {
    return {StreamDispatchCode::UnknownStream, 0, 0};
}

StreamDispatchResult
StreamRuntime::on_data(const net::InboundFrameContext& /*ictx*/,
                       const net::StreamDataFrame& /*data*/) noexcept {
    return {StreamDispatchCode::UnknownStream, 0, 0};
}

StreamDispatchResult
StreamRuntime::on_ack(const net::InboundFrameContext& /*ictx*/,
                      const net::StreamAckFrame& /*ack*/) noexcept {
    return {StreamDispatchCode::UnknownStream, 0, 0};
}

StreamDispatchResult
StreamRuntime::on_close(const net::InboundFrameContext& /*ictx*/,
                        const net::StreamCloseFrame& /*close*/) noexcept {
    return {StreamDispatchCode::UnknownStream, 0, 0};
}

StreamDispatchResult
StreamRuntime::on_error(const net::InboundFrameContext& /*ictx*/,
                        const net::StreamErrorFrame& /*error*/) noexcept {
    return {StreamDispatchCode::UnknownStream, 0, 0};
}

// ── Facade API ─────────────────────────────────────────────────────────────

void StreamRuntime::register_sender(uint64_t stream_id, ActorId actor_id) {
    // Compatibility: local-only stream key from Phase 3 StreamRegistry
    // Phase 4 peer-qualified keys will replace this in Task 9
    (void)stream_id;
    (void)actor_id;
}

void StreamRuntime::register_receiver(uint64_t stream_id, ActorId actor_id) {
    (void)stream_id;
    (void)actor_id;
}

void StreamRuntime::unregister_stream(uint64_t stream_id) {
    (void)stream_id;
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
    return snap;
}

} // namespace hpactor
