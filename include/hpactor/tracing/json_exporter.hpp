#pragma once

#include <hpactor/tracing/trace_exporter.hpp>

#include <fstream>
#include <mutex>
#include <string>

namespace hpactor::tracing {

class JsonFileExporter final : public SpanExporter {
  public:
    explicit JsonFileExporter(std::string path);
    result<void> export_batch(std::span<const SpanRecord> batch) noexcept override;
    void shutdown() noexcept override;
    const char* name() const noexcept override {
        return "json_file";
    }

  private:
    std::string path_;
    std::ofstream out_;
    std::mutex mutex_;
};

std::string span_record_to_json(const SpanRecord& record);

} // namespace hpactor::tracing
