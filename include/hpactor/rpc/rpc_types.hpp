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

#include <hpactor/types/types.hpp>

namespace hpactor {

/// \brief An RPC response frame delivered from the transport layer to
///        \c RpcChannel.
///
/// Carries the correlation \c MessageId, the response payload, and
/// optional distributed trace context propagated from the responder.
struct RpcResponseFrame {
    /// \brief Correlation id matching the original RPC request.
    MessageId msg_id;

    /// \brief Serialized response payload.
    ///
    /// Ownership: the transport layer populates this buffer. The
    /// receiver (typically \c RpcChannel::on_response()) moves the
    /// data into the corresponding \c PendingCall promise.
    StreamBuffer payload;

    /// \brief Whether \c trace_context contains valid trace data
    ///        propagated from the responder.
    bool has_trace_context{false};

    /// \brief Trace context propagated from the responder for
    ///        distributed tracing span correlation.
    TraceContext trace_context{};
};

} // namespace hpactor
