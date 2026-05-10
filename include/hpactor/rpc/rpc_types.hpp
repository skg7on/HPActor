#pragma once

#include <hpactor/types/types.hpp>

namespace hpactor {

struct RpcResponseFrame {
    MessageId msg_id;
    StreamBuffer payload;
    bool has_trace_context{false};
    TraceContext trace_context{};
};

} // namespace hpactor
