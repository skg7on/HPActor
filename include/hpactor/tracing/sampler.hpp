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
