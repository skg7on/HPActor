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

#include <atomic>
#include <cstdint>

namespace hpactor::tracing {

struct SamplingParameters {
    TraceId trace_id;
    bool has_parent{false};
    bool parent_sampled{false};
};

struct SamplingDecision {
    bool sampled{false};
};

class Sampler {
  public:
    virtual ~Sampler() = default;
    virtual SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept = 0;
};

class AlwaysOnSampler final : public Sampler {
  public:
    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;
};

class AlwaysOffSampler final : public Sampler {
  public:
    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;
};

class TraceIdRatioSampler final : public Sampler {
  public:
    explicit TraceIdRatioSampler(double ratio);
    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;

  private:
    double ratio_{0.0};
};

class ParentBasedSampler final : public Sampler {
  public:
    explicit ParentBasedSampler(double root_ratio);
    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;

  private:
    TraceIdRatioSampler root_sampler_;
};

class TraceIdGenerator {
  public:
    TraceIdGenerator() = default;
    TraceId next_trace_id() noexcept;
    SpanId next_span_id() noexcept;

  private:
    std::atomic<uint64_t> counter_{1};
};

} // namespace hpactor::tracing