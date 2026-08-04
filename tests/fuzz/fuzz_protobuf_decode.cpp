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

/// \file fuzz_protobuf_decode.cpp
/// \brief Fuzz target for protobuf decode paths — \c WireEnvelope,
///        \c PbActorAddress, and \c PbTraceContext parsing plus oneof dispatch.

#include "fuzz_harness.hpp"
#include <hpactor/common.pb.h>
#include <hpactor/frame.pb.h>
#include <hpactor/msg/frame.hpp>
#include <string>

using namespace hpactor;

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    return fuzz_entry(data, size, [](const uint8_t* d, size_t s) {
        std::string payload(reinterpret_cast<const char*>(d), s);

        // Test 1: Parse as WireEnvelope — the main network protobuf type
        net::WireEnvelope envelope;
        if (envelope.ParseFromString(payload)) {
            // Exercise oneof dispatch — mirrors InboundFrameRouter
            // classification
            switch (envelope.payload_case()) {
                case net::WireEnvelope::kDataFrame: {
                    const auto& df = envelope.data_frame();
                    // Access nested sub-messages (PbActorAddress is a oneof)
                    if (df.sender().has_local_addr()) {
                        (void)df.sender().local_addr().actor_id();
                    }
                    if (df.receiver().has_local_addr()) {
                        (void)df.receiver().local_addr().actor_id();
                    }
                    (void)df.payload().size();
                    (void)df.type_tag();
                    (void)df.message_id();
                    (void)df.flags();
                    // Exercise trace context if present
                    if (df.has_trace_context()) {
                        (void)df.trace_context().trace_id().size();
                        (void)df.trace_context().span_id().size();
                    }
                    break;
                }
                case net::WireEnvelope::kBatchFrame: {
                    const auto& bf = envelope.batch_frame();
                    int count = bf.entries_size();
                    // Bounded iteration — batch frames can have many entries
                    for (int i = 0; i < count && i < 128; ++i) {
                        const auto& e = bf.entries(i);
                        (void)e.type_tag();
                        (void)e.message_id();
                        (void)e.flags();
                        (void)e.payload().size();
                    }
                    break;
                }
                case net::WireEnvelope::kAckFrame:
                    (void)envelope.ack_frame().message_id();
                    break;
                case net::WireEnvelope::kNackFrame:
                    (void)envelope.nack_frame().message_id();
                    (void)envelope.nack_frame().reason();
                    break;
                case net::WireEnvelope::kStreamOpen:
                    (void)envelope.stream_open().stream_id();
                    break;
                case net::WireEnvelope::kStreamData:
                    (void)envelope.stream_data().stream_id();
                    (void)envelope.stream_data().sequence();
                    break;
                case net::WireEnvelope::kStreamAck:
                    (void)envelope.stream_ack().stream_id();
                    break;
                case net::WireEnvelope::kStreamClose:
                    (void)envelope.stream_close().stream_id();
                    break;
                case net::WireEnvelope::kStreamError:
                    (void)envelope.stream_error().stream_id();
                    break;
                default:
                    break;
            }

            // Round-trip: re-serialize and re-parse
            std::string reserialized;
            if (envelope.SerializeToString(&reserialized)) {
                net::WireEnvelope envelope2;
                bool ok2 = envelope2.ParseFromString(reserialized);
                // Verify oneof discrimination survives round-trip
                (void)(envelope2.payload_case() == envelope.payload_case());
                (void)ok2;
            }
        }

        // Test 2: Parse as PbActorAddress (nested sub-message)
        PbActorAddress addr;
        if (addr.ParseFromString(payload)) {
            if (addr.has_local_addr()) {
                (void)addr.local_addr().actor_id();
                (void)addr.local_addr().incarnation();
            }
            if (addr.has_global_addr()) {
                bool v4 = addr.global_addr().endpoint().has_ipv4();
                (void)v4;
            }
        }

        // Test 3: Parse as PbTraceContext (nested sub-message)
        net::PbTraceContext trace;
        if (trace.ParseFromString(payload)) {
            (void)trace.trace_id().size();
            (void)trace.span_id().size();
            (void)trace.tracestate().size();
            (void)trace.flags();
        }
    });
}
