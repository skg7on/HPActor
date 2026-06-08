// Copyright 2026 HPActor Contributors
// Licensed under the Apache License, Version 2.0

#include <hpactor/actor/actor_directory.hpp>
#include <hpactor/mailbox/backpressure_coordinator.hpp>
#include <hpactor/mailbox/backpressure_signal_serialization.hpp>
#include <hpactor/metrics/metrics_ring_buffer.hpp>
#include <hpactor/net/tcp_transport.hpp>

namespace hpactor {

namespace {
using MetricBuf = metrics::MpscRingBuffer<metrics::MetricEvent>;
} // namespace

BackpressureCoordinator::BackpressureCoordinator(Config config)
    : config_(std::move(config)) {}

BackpressureCoordinator::~BackpressureCoordinator() = default;

void BackpressureCoordinator::emit_local_signal(
    const mailbox::BackpressureSignal& signal, mailbox::MailboxPressureState state) {
    auto* metrics = static_cast<MetricBuf*>(config_.metrics_ring_buffer);
    if (metrics) {
        metrics::MetricEvent evt{};
        evt.actor_id = signal.target.id;
        evt.event_type = metrics::MetricEventType::kBackpressureSignal;
        evt.code = static_cast<uint8_t>(signal.reason);
        evt.aux = static_cast<uint8_t>(state);
        evt.value_hi = 1;
        metrics->try_push(evt);
    }
    deliver_to_sender(signal);
}

void BackpressureCoordinator::emit_remote_signal(
    const mailbox::BackpressureSignal& signal, mailbox::MailboxPressureState state) {
    auto payload = mailbox::serialize_backpressure_signal(signal, state);
    if (payload.empty())
        return;

    auto* metrics = static_cast<MetricBuf*>(config_.metrics_ring_buffer);
    if (metrics) {
        metrics::MetricEvent evt{};
        evt.actor_id = signal.target.id;
        evt.event_type = metrics::MetricEventType::kBackpressureSignal;
        evt.code = static_cast<uint8_t>(signal.reason);
        evt.aux = static_cast<uint8_t>(state);
        evt.value_hi = 1;
        metrics->try_push(evt);
    }

    net::WireFrame frame;
    net::to_proto(frame.pb_frame.mutable_sender(), signal.target);
    net::to_proto(frame.pb_frame.mutable_receiver(), signal.sender);
    frame.pb_frame.set_type_tag(
        static_cast<uint32_t>(TypeTag::BackpressureSignalTag));
    frame.pb_frame.set_message_id(signal.sequence);
    frame.pb_frame.set_payload(reinterpret_cast<const char*>(payload.data()),
                               payload.size());

    auto encoded = frame.encode();
    if (wire_sink_for_test_) {
        (void)wire_sink_for_test_(signal.sender, encoded);
    } else if (config_.transport) {
        (void)config_.transport->try_send(signal.sender, encoded);
    }
}

bool BackpressureCoordinator::handle_remote_signal(const net::WireFrame& frame) {
    StreamBuffer payload(frame.pb_frame.payload().begin(),
                         frame.pb_frame.payload().end());
    auto decoded = mailbox::deserialize_backpressure_signal(payload);
    if (!decoded.has_value())
        return false;
    deliver_to_sender(decoded->signal);
    return true;
}

void BackpressureCoordinator::deliver_to_sender(const mailbox::BackpressureSignal& signal) {
    if (signal.sender.id == ActorId{0})
        return;
    if (!config_.actor_directory)
        return;
    auto ctx = config_.actor_directory->find_context(signal.sender.id);
    if (ctx)
        ctx->handle_backpressure(signal);
}

void BackpressureCoordinator::set_wire_sink_for_test(WireSink sink) {
    wire_sink_for_test_ = std::move(sink);
}

} // namespace hpactor
