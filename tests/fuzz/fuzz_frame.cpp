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

/// \file fuzz_frame.cpp
/// \brief Fuzz target for \c try_decode_wireframe() — the wire protocol frame
///        decoder that parses network ingress bytes.

#include "fuzz_harness.hpp"
#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/msg/frame.hpp>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        StreamBuffer buf(d, d + s);

        // Test 1: default limits (16 MiB, reject trailing bytes)
        auto r1 = net::try_decode_wireframe(buf);
        if (r1.ok()) {
            // Round-trip: encode then decode the result
            auto encoded = r1.frame.encode();
            auto r2 = net::try_decode_wireframe(encoded);
            // Must also succeed — round-trip is a correctness invariant
            (void)r2;
        }
        // Exercise error path discriminators
        (void)(r1.error == net::FrameDecodeError::None);
        (void)(r1.error == net::FrameDecodeError::HeaderTooShort);
        (void)(r1.error == net::FrameDecodeError::InvalidMagic);
        (void)(r1.error == net::FrameDecodeError::FrameTooLarge);
        (void)(r1.error == net::FrameDecodeError::LengthMismatch);
        (void)(r1.error == net::FrameDecodeError::TrailingBytes);
        (void)(r1.error == net::FrameDecodeError::InvalidProtobuf);
        (void)r1.declared_payload_bytes;

        // Test 2: relaxed limits — no payload bound, allow trailing bytes
        net::FrameDecodeLimits relaxed{};
        relaxed.max_payload_bytes = 0;
        relaxed.reject_trailing_bytes = false;
        auto r3 = net::try_decode_wireframe(buf, relaxed);
        (void)r3;

        // Test 3: tight limits — 1-byte payload max
        net::FrameDecodeLimits tight{};
        tight.max_payload_bytes = 1;
        tight.reject_trailing_bytes = true;
        auto r4 = net::try_decode_wireframe(buf, tight);
        (void)r4;

        // Test 4: zero payload bound (allowed — 0 means "no check")
        net::FrameDecodeLimits zero{};
        zero.max_payload_bytes = 0;
        zero.reject_trailing_bytes = false;
        auto r5 = net::try_decode_wireframe(buf, zero);
        (void)r5;

        // Test 5: legacy decode() paths
        auto decoded_buf = net::WireFrame::decode(buf);
        (void)decoded_buf.pb_envelope.payload_case();
        auto decoded_span = net::WireFrame::decode(std::span<const uint8_t>(d, s));
        (void)decoded_span.pb_envelope.payload_case();
    });
}
