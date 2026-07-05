// Copyright 2026 HPActor Contributors
// (Apache 2.0 license header)
#pragma once

#include <atomic>
#include <cstdint>

#include <hpactor/metrics/metrics_event.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/msg/frame.hpp>
#include <hpactor/net/frame_dispatch_result.hpp>
#include <hpactor/net/inbound_frame_sink.hpp>
#include <hpactor/rpc/rpc_channel.hpp>
#include <hpactor/types/types.hpp>

#include <hpactor/actor/stream/stream_runtime.hpp>
#include <hpactor/runtime/messaging_runtime.hpp>

namespace hpactor {

namespace net {

/// \brief Sole classifier of valid inbound HPActor envelopes.
///
/// Owns no transport, actor, message queue, stream map, or RPC pending
/// state.  Routes by protobuf oneof and delegates to concrete owners:
/// \c MessagingRuntime, \c RpcChannel, and \c StreamRuntime.
///
/// Installed into \c ConnectionPool via the fixed \c InboundFrameSink
/// returned by \c inbound_sink().  Idempotent \c disable() stops routing
/// for shutdown without destroying the object.
///
/// \note Thread safety: Called from event-loop callback thread(s).
///       Internal state is limited to one atomic accepting flag.
class InboundFrameRouter final : public InboundFrameTarget {
  public:
    struct Config {
        uint32_t max_batch_entries{1024};
        uint16_t max_tracestate_len{256};
        uint16_t rpc_max_tracestate_len{256};
    };

    struct Dependencies {
        MessagingRuntime& messaging;
        RpcChannel& rpc;
        StreamRuntime& streams;
        metrics::MpscRingBuffer<metrics::MetricEvent>* metrics{nullptr};
    };

    InboundFrameRouter(Dependencies dependencies, Config config) noexcept;

    InboundFrameRouter(const InboundFrameRouter&) = delete;
    InboundFrameRouter& operator=(const InboundFrameRouter&) = delete;

    // ── InboundFrameTarget interface ──

    /// \brief Classify and route one valid frame.
    FrameDispatchResult on_frame(const InboundFrameContext& ictx,
                                 const WireFrame& frame) noexcept override;

    /// \brief Handle a decode failure with the typed error reason.
    FrameDispatchResult on_decode_failure(const InboundFrameContext& ictx,
                                          FrameDecodeError error) noexcept override;

    /// \brief Return a typed-target sink referencing \c this stable object.
    [[nodiscard]] InboundFrameSink inbound_sink() noexcept {
        return InboundFrameSink{this};
    }

    /// \brief Idempotent: stop accepting new frames for shutdown.
    void disable() noexcept;

  private:
    /// Common message builder: validates address, constructs TypedMessage,
    /// parses trace, sets metadata, then calls full messaging delivery.
    FrameDispatchResult
    deliver_ordinary_data(const InboundFrameContext& ictx, const WireFrame& frame,
                          const ActorMsgFrame& data) noexcept;

    FrameDispatchResult route_data_payload(const InboundFrameContext& ictx,
                                           const WireFrame& frame) noexcept;

    FrameDispatchResult route_dedicated_ack(const WireFrame& frame) noexcept;
    FrameDispatchResult route_dedicated_nack(const WireFrame& frame) noexcept;
    FrameDispatchResult
    route_batch(const InboundFrameContext& ictx, const WireFrame& frame) noexcept;

    Config config_;
    MessagingRuntime& messaging_;
    RpcChannel& rpc_;
    StreamRuntime& streams_;
    [[maybe_unused]] metrics::MpscRingBuffer<metrics::MetricEvent>* metrics_;
    std::atomic<bool> accepting_{true};
};

} // namespace net
} // namespace hpactor
