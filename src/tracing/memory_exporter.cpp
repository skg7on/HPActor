#include <hpactor/tracing/memory_exporter.hpp>

namespace hpactor::tracing {

result<void>
MemoryExporter::export_batch(std::span<const SpanRecord> batch) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    spans_.insert(spans_.end(), batch.begin(), batch.end());
    return result<void>::make();
}

std::vector<SpanRecord> MemoryExporter::snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return spans_;
}

void MemoryExporter::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    spans_.clear();
}

} // namespace hpactor::tracing
