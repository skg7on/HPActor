#pragma once

#include <hpactor/tracing/span.hpp>
#include <hpactor/types/types.hpp>

#include <span>

namespace hpactor::tracing {

class SpanExporter {
  public:
    virtual ~SpanExporter() = default;
    virtual result<void>
    export_batch(std::span<const SpanRecord> batch) noexcept = 0;
    virtual void shutdown() noexcept = 0;
    virtual const char* name() const noexcept = 0;
};

} // namespace hpactor::tracing
