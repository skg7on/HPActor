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

/// \brief Parameters passed to a Sampler to make a sampling decision.
struct SamplingParameters {
    /// \brief The trace id under consideration.
    TraceId trace_id;
    /// \brief Whether a parent span context exists.
    bool has_parent{false};
    /// \brief Parent's sampling flag (only meaningful when has_parent=true).
    bool parent_sampled{false};
};

/// \brief Result of a sampling decision.
struct SamplingDecision {
    /// \brief Whether the span should be recorded and exported.
    bool sampled{false};
};

/// \brief Abstract interface for trace sampling decisions.
///
/// \note Thread safety: should_sample() must be safe for concurrent access.
class Sampler {
  public:
    virtual ~Sampler() = default;

    /// \brief Decide whether a span should be sampled.
    ///
    /// \param[in] params Sampling parameters including trace id and parent
    ///                   decision.
    /// \return The sampling decision.
    virtual SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept = 0;
};

/// \brief Sampler that always records every span.
class AlwaysOnSampler final : public Sampler {
  public:
    /// \brief Always returns sampled=true.
    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;
};

/// \brief Sampler that never records any span.
class AlwaysOffSampler final : public Sampler {
  public:
    /// \brief Always returns sampled=false.
    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;
};

/// \brief Sampler that decides based on a fixed ratio of trace ids.
///
/// Hashes the trace id and compares it to the configured ratio threshold.
class TraceIdRatioSampler final : public Sampler {
  public:
    /// \brief Construct with a sampling ratio.
    ///
    /// \param[in] ratio Fraction of traces to sample (0.0–1.0).
    explicit TraceIdRatioSampler(double ratio);

    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;

  private:
    double ratio_{0.0};
};

/// \brief Sampler that respects parent decision with ratio-based root sampling.
///
/// If the span has a parent, honors the parent's sampling decision.
/// For root spans, delegates to a TraceIdRatioSampler.
class ParentBasedSampler final : public Sampler {
  public:
    /// \brief Construct with a ratio for root spans.
    ///
    /// \param[in] root_ratio Sampling ratio for root spans (0.0–1.0).
    explicit ParentBasedSampler(double root_ratio);

    SamplingDecision
    should_sample(const SamplingParameters& params) const noexcept override;

  private:
    TraceIdRatioSampler root_sampler_;
};

/// \brief Monotonically increasing id generator for trace and span ids.
///
/// \note Thread safety: lock-free via std::atomic. Safe for concurrent access.
class TraceIdGenerator {
  public:
    TraceIdGenerator() = default;

    /// \brief Generate the next trace id.
    ///
    /// \return A globally unique trace id for this process.
    TraceId next_trace_id() noexcept;

    /// \brief Generate the next span id.
    ///
    /// \return A globally unique span id for this process.
    SpanId next_span_id() noexcept;

  private:
    std::atomic<uint64_t> counter_{1};
};

} // namespace hpactor::tracing
