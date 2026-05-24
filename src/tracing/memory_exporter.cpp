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