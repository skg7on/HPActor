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

#include <hpactor/mailbox/backpressure_signal_serialization.hpp>

#include <hpactor/messages.pb.h>
#include <hpactor/msg/frame.hpp>

namespace hpactor::mailbox {

namespace {

MailboxPressureState pressure_state_from_u32(uint32_t value) noexcept {
    switch (static_cast<MailboxPressureState>(value)) {
        case MailboxPressureState::Normal:
        case MailboxPressureState::SoftPressure:
        case MailboxPressureState::HardPressure:
        case MailboxPressureState::Recovering:
            return static_cast<MailboxPressureState>(value);
    }
    return MailboxPressureState::Normal;
}

BackpressureReason reason_from_u32(uint32_t value) noexcept {
    switch (static_cast<BackpressureReason>(value)) {
        case BackpressureReason::HighWatermark:
        case BackpressureReason::HardCapacity:
        case BackpressureReason::ByteCapacity:
        case BackpressureReason::OverflowPolicy:
        case BackpressureReason::NodeMemoryPressure:
            return static_cast<BackpressureReason>(value);
    }
    return BackpressureReason::HighWatermark;
}

} // namespace

StreamBuffer serialize_backpressure_signal(const BackpressureSignal& signal,
                                           MailboxPressureState state) {
    ::hpactor::BackpressureSignalMessage pb;
    net::to_proto(pb.mutable_target(), signal.target);
    net::to_proto(pb.mutable_sender(), signal.sender);
    pb.set_reason(static_cast<uint32_t>(signal.reason));
    pb.set_pressure_state(static_cast<uint32_t>(state));
    pb.set_depth(signal.depth);
    pb.set_capacity(signal.capacity);
    pb.set_bytes(signal.bytes);
    pb.set_byte_capacity(signal.byte_capacity);
    pb.set_pressure_ratio(signal.pressure_ratio);
    pb.set_retry_after_ms(static_cast<uint64_t>(signal.retry_after.count()));
    pb.set_sequence(signal.sequence);

    StreamBuffer out(pb.ByteSizeLong());
    if (!pb.SerializeToArray(out.data(), static_cast<int>(out.size()))) {
        out.clear();
    }
    return out;
}

std::optional<DecodedBackpressureSignal>
deserialize_backpressure_signal(const StreamBuffer& payload) {
    ::hpactor::BackpressureSignalMessage pb;
    if (!pb.ParseFromArray(payload.data(), static_cast<int>(payload.size()))) {
        return std::nullopt;
    }

    DecodedBackpressureSignal decoded;
    decoded.signal.target = net::from_proto(pb.target());
    decoded.signal.sender = net::from_proto(pb.sender());
    decoded.signal.reason = reason_from_u32(pb.reason());
    decoded.state = pressure_state_from_u32(pb.pressure_state());
    decoded.signal.depth = pb.depth();
    decoded.signal.capacity = pb.capacity();
    decoded.signal.bytes = pb.bytes();
    decoded.signal.byte_capacity = pb.byte_capacity();
    decoded.signal.pressure_ratio = pb.pressure_ratio();
    decoded.signal.retry_after = std::chrono::milliseconds(pb.retry_after_ms());
    decoded.signal.sequence = pb.sequence();
    return decoded;
}

} // namespace hpactor::mailbox
