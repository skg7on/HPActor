#pragma once

#include <hpactor/tracing/trace_exporter.hpp>

#include <string>

namespace hpactor::tracing {

class OtlpHttpExporter final : public SpanExporter {
  public:
    explicit OtlpHttpExporter(std::string endpoint);
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;
    void shutdown() noexcept override {}
    const char* name() const noexcept override {
        return "otlp_http";
    }

    std::string build_json_payload_for_test(std::span<const SpanRecord> batch,
                                            const std::string& service_name) const;

  private:
    std::string endpoint_;
};

} // namespace hpactor::tracing
