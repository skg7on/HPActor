#include <hpactor/tracing/sampler.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>

namespace hpactor::tracing {

namespace {

uint64_t splitmix64(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

uint64_t trace_low64(const TraceId& id) noexcept {
    uint64_t value = 0;
    for (size_t i = 8; i < 16; ++i) {
        value = (value << 8) | id.bytes[i];
    }
    return value;
}

} // namespace

SamplingDecision
AlwaysOnSampler::should_sample(const SamplingParameters& /*params*/) const noexcept {
    return {true};
}

SamplingDecision
AlwaysOffSampler::should_sample(const SamplingParameters& /*params*/) const noexcept {
    return {false};
}

TraceIdRatioSampler::TraceIdRatioSampler(double ratio)
    : ratio_(std::clamp(ratio, 0.0, 1.0)) {}

SamplingDecision
TraceIdRatioSampler::should_sample(const SamplingParameters& params) const noexcept {
    if (ratio_ <= 0.0)
        return {false};
    if (ratio_ >= 1.0)
        return {true};
    const uint64_t threshold =
        static_cast<uint64_t>(ratio_ * static_cast<double>(UINT64_MAX));
    return {splitmix64(trace_low64(params.trace_id)) <= threshold};
}

ParentBasedSampler::ParentBasedSampler(double root_ratio)
    : root_sampler_(root_ratio) {}

SamplingDecision
ParentBasedSampler::should_sample(const SamplingParameters& params) const noexcept {
    if (params.has_parent) {
        return {params.parent_sampled};
    }
    return root_sampler_.should_sample(params);
}

TraceId TraceIdGenerator::next_trace_id() noexcept {
    TraceId id;
    const uint64_t n = counter_.fetch_add(1, std::memory_order_relaxed);
    uint64_t hi = splitmix64(n);
    uint64_t lo = splitmix64(n ^ 0xd1b54a32d192ed03ULL);
    for (int i = 7; i >= 0; --i) {
        id.bytes[static_cast<size_t>(7 - i)] =
            static_cast<uint8_t>((hi >> (i * 8)) & 0xFF);
        id.bytes[static_cast<size_t>(15 - i)] =
            static_cast<uint8_t>((lo >> (i * 8)) & 0xFF);
    }
    if (!id.valid()) {
        id.bytes[15] = 1;
    }
    return id;
}

SpanId TraceIdGenerator::next_span_id() noexcept {
    SpanId id;
    const uint64_t n = counter_.fetch_add(1, std::memory_order_relaxed);
    uint64_t value = splitmix64(n ^ 0x94d049bb133111ebULL);
    for (int i = 7; i >= 0; --i) {
        id.bytes[static_cast<size_t>(7 - i)] =
            static_cast<uint8_t>((value >> (i * 8)) & 0xFF);
    }
    if (!id.valid()) {
        id.bytes[7] = 1;
    }
    return id;
}

} // namespace hpactor::tracing
