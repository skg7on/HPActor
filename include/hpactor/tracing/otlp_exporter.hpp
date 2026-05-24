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