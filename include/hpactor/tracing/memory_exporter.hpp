#pragma once

#include <hpactor/tracing/trace_exporter.hpp>

#include <mutex>
#include <vector>

namespace hpactor::tracing {

class MemoryExporter final : public SpanExporter {
  public:
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;
    void shutdown() noexcept override {}
    const char* name() const noexcept override {
        return "memory";
    }
    std::vector<SpanRecord> snapshot() const;
    void clear();

  private:
    mutable std::mutex mutex_;
    std::vector<SpanRecord> spans_;
};

} // namespace hpactor::tracing
