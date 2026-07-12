// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <unordered_map>

#include <hpactor/actor/stream/stream_config.hpp>
#include <hpactor/actor/stream/stream_handle.hpp>
#include <hpactor/actor/stream/stream_snapshot.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/inbound_frame_sink.hpp>
#include <hpactor/types/types.hpp>

namespace hpactor {

class MessagingRuntime;

/// \brief Dispatch result from a stream frame handler.
enum class StreamDispatchCode : uint8_t {
    Handled,
    UnknownStream,
    DuplicateStream,
    CapacityExceeded,
    InvalidPeer,
    InvalidFrame,
    SpawnFailed,
    DeliveryRejected,
    MetadataDropped,
};

/// \brief Fixed-size stream handler outcome.
struct StreamDispatchResult {
    StreamDispatchCode code{StreamDispatchCode::Handled};
    uint32_t accepted_count{0};
    uint32_t rejected_count{0};
};

/// \brief Narrow port for stream actor lifecycle operations.
///
/// No \c ActorSystem capture — only function-pointer/context pairs.
struct StreamActorLifecyclePort {
    void* context{nullptr};
    result<ActorId> (*spawn_sender)(void*, ActorId target, uint64_t stream_id,
                                    const StreamConfig&,
                                    const TraceContext&) noexcept {nullptr};
    result<ActorId> (*spawn_receiver)(void*, ActorId target, uint64_t stream_id,
                                      const ActorAddress& sender, EndPoint peer,
                                      uint32_t initial_window_bytes,
                                      const TraceContext&) noexcept {nullptr};
    void (*stop)(void*, ActorId) noexcept {nullptr};
};

/// \brief Cohesive owner of peer-qualified bounded stream session state.
///
/// Owns one mutex-protected session map keyed by \c (peer, stream_id).
/// The mutex protects only keys, registration state, capacity, and
/// snapshot copying.  No actor spawn, delivery, wire output, logging,
/// or metric emission occurs while the lock is held.
class StreamRuntime final {
  public:
    struct Config {
        uint32_t max_active_streams{4096};
        uint16_t max_tracestate_len{256};
    };

    StreamRuntime(Config config, StreamActorLifecyclePort actor_port,
                  MessagingRuntime& messaging) noexcept;

    StreamRuntime(const StreamRuntime&) = delete;
    StreamRuntime& operator=(const StreamRuntime&) = delete;

    // ── Stream frame handlers (called from InboundFrameRouter) ───────────

    StreamDispatchResult on_open(const net::InboundFrameContext& ictx,
                                 const net::StreamOpenFrame& open) noexcept;

    StreamDispatchResult on_data(const net::InboundFrameContext& ictx,
                                 const net::StreamDataFrame& data) noexcept;

    StreamDispatchResult on_ack(const net::InboundFrameContext& ictx,
                                const net::StreamAckFrame& ack) noexcept;

    StreamDispatchResult on_close(const net::InboundFrameContext& ictx,
                                  const net::StreamCloseFrame& close) noexcept;

    StreamDispatchResult on_error(const net::InboundFrameContext& ictx,
                                  const net::StreamErrorFrame& error) noexcept;

    // ── Local (facade-forward) API ───────────────────────────────────────

    void register_sender(uint64_t stream_id, ActorId actor_id);
    /// Register a sender with an explicit peer endpoint (for cross-process).
    void register_sender_for_peer(EndPoint peer, uint64_t stream_id,
                                   ActorId actor_id);
    void register_receiver(uint64_t stream_id, ActorId actor_id);
    void unregister_stream(uint64_t stream_id);
    uint64_t allocate_stream_id(ActorId sender_id);
    std::optional<StreamHandle>
    open_stream(ActorId target, const StreamConfig& config);

    // ── Observability ────────────────────────────────────────────────────

    StreamRuntimeSnapshot snapshot() const;

  private:
    Config config_;
    StreamActorLifecyclePort actor_port_;
    MessagingRuntime& messaging_;

    mutable std::mutex mutex_;
    std::unordered_map<StreamKey, StreamSession> sessions_;
    std::atomic<uint64_t> counter_{1};
};

} // namespace hpactor
