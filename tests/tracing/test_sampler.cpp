#include <hpactor/tracing/sampler.hpp>

#include <cassert>

using namespace hpactor;
using namespace hpactor::tracing;

static TraceId make_trace(uint8_t last) {
    TraceId id;
    id.bytes[15] = last;
    return id;
}

int main() {
    SamplingParameters params;
    params.trace_id = make_trace(1);

    AlwaysOnSampler on;
    assert(on.should_sample(params).sampled);

    AlwaysOffSampler off;
    assert(!off.should_sample(params).sampled);

    TraceIdRatioSampler zero(0.0);
    assert(!zero.should_sample(params).sampled);

    TraceIdRatioSampler one(1.0);
    assert(one.should_sample(params).sampled);

    ParentBasedSampler parent_on(0.0);
    params.has_parent = true;
    params.parent_sampled = true;
    assert(parent_on.should_sample(params).sampled);

    params.parent_sampled = false;
    assert(!parent_on.should_sample(params).sampled);
    return 0;
}
