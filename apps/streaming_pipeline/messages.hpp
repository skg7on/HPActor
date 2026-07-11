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

#include <hpactor/adt/stream_buffer.hpp>
#include <hpactor/types/types.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace hpactor::apps::streaming_pipeline {

// ── TypeTags ────────────────────────────────────────────────────────────────

/// Application-level TypeTags (extension range 0x00030000+).
inline constexpr TypeTag SensorReadingTag{0x00030000};
inline constexpr TypeTag TransformedChunkTag{0x00030001};
inline constexpr TypeTag PipelineControlTag{0x00030002};
inline constexpr TypeTag PipelineStatsTag{0x00030003};

// ── Data Structures ──────────────────────────────────────────────────────────

/// A single sensor reading: timestamp + value.
struct SensorReading {
    uint64_t timestamp_ns = 0;
    double value = 0.0;
};

/// A batch of sensor readings sent as one stream chunk.
struct SensorBatch {
    std::string sensor_id;
    uint64_t batch_seq = 0;
    std::vector<SensorReading> readings;
};

/// Transformed / aggregated output from the processor.
struct TransformedChunk {
    std::string sensor_id;
    uint64_t batch_seq = 0;
    uint64_t input_count = 0;
    double min_value = 0.0;
    double max_value = 0.0;
    double mean_value = 0.0;
    double stddev = 0.0;
};

/// Pipeline control: kick a scenario.
struct PipelineKick {
    std::string scenario;
};

/// Final pipeline statistics delivered on close.
struct PipelineStats {
    uint64_t total_chunks = 0;
    uint64_t total_readings = 0;
    uint64_t total_bytes = 0;
    double total_min = 0.0;
    double total_max = 0.0;
};

// ── Serialization (hand-rolled binary, same pattern as order_platform) ──────

class BufferWriter {
  public:
    void u8(uint8_t value) { buf_.push_back(value); }

    void u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            buf_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            buf_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void f64(double value) {
        uint64_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void str(const std::string& s) {
        u32(static_cast<uint32_t>(s.size()));
        auto begin = reinterpret_cast<const uint8_t*>(s.data());
        buf_.insert(buf_.end(), begin, begin + s.size());
    }

    void raw(const uint8_t* data, size_t len) {
        buf_.insert(buf_.end(), data, data + len);
    }

    StreamBuffer finish() { return std::move(buf_); }

  private:
    StreamBuffer buf_;
};

class BufferReader {
  public:
    explicit BufferReader(const StreamBuffer& buffer) : buf_(buffer) {}

    bool u8(uint8_t& v) {
        if (off_ + 1 > buf_.size()) return false;
        v = buf_[off_++];
        return true;
    }

    bool u32(uint32_t& v) {
        if (off_ + 4 > buf_.size()) return false;
        v = 0;
        for (int i = 0; i < 4; ++i) v = (v << 8) | buf_[off_++];
        return true;
    }

    bool u64(uint64_t& v) {
        if (off_ + 8 > buf_.size()) return false;
        v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | buf_[off_++];
        return true;
    }

    bool f64(double& v) {
        uint64_t bits = 0;
        if (!u64(bits)) return false;
        std::memcpy(&v, &bits, sizeof(v));
        return true;
    }

    bool str(std::string& s) {
        uint32_t len = 0;
        if (!u32(len)) return false;
        if (off_ + len > buf_.size()) return false;
        s.assign(reinterpret_cast<const char*>(buf_.data() + off_), len);
        off_ += len;
        return true;
    }

    bool done() const { return off_ == buf_.size(); }

  private:
    const StreamBuffer& buf_;
    size_t off_ = 0;
};

// ── Encoders / Decoders ─────────────────────────────────────────────────────

inline StreamBuffer encode_sensor_batch(const SensorBatch& batch) {
    BufferWriter w;
    w.str(batch.sensor_id);
    w.u64(batch.batch_seq);
    w.u32(static_cast<uint32_t>(batch.readings.size()));
    for (const auto& r : batch.readings) {
        w.u64(r.timestamp_ns);
        w.f64(r.value);
    }
    return w.finish();
}

inline bool decode_sensor_batch(const StreamBuffer& buf, SensorBatch& out) {
    BufferReader r(buf);
    uint32_t count = 0;
    if (!r.str(out.sensor_id)) return false;
    if (!r.u64(out.batch_seq)) return false;
    if (!r.u32(count)) return false;
    out.readings.clear();
    out.readings.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        SensorReading sr;
        if (!r.u64(sr.timestamp_ns)) return false;
        if (!r.f64(sr.value)) return false;
        out.readings.push_back(sr);
    }
    return r.done();
}

inline StreamBuffer encode_transformed(const TransformedChunk& tc) {
    BufferWriter w;
    w.str(tc.sensor_id);
    w.u64(tc.batch_seq);
    w.u64(tc.input_count);
    w.f64(tc.min_value);
    w.f64(tc.max_value);
    w.f64(tc.mean_value);
    w.f64(tc.stddev);
    return w.finish();
}

inline bool decode_transformed(const StreamBuffer& buf, TransformedChunk& out) {
    BufferReader r(buf);
    if (!r.str(out.sensor_id)) return false;
    if (!r.u64(out.batch_seq)) return false;
    if (!r.u64(out.input_count)) return false;
    if (!r.f64(out.min_value)) return false;
    if (!r.f64(out.max_value)) return false;
    if (!r.f64(out.mean_value)) return false;
    if (!r.f64(out.stddev)) return false;
    return r.done();
}

inline StreamBuffer encode_control(const PipelineKick& ctrl) {
    BufferWriter w;
    w.str(ctrl.scenario);
    return w.finish();
}

inline bool decode_control(const StreamBuffer& buf, PipelineKick& out) {
    BufferReader r(buf);
    return r.str(out.scenario) && r.done();
}

inline StreamBuffer encode_stats(const PipelineStats& stats) {
    BufferWriter w;
    w.u64(stats.total_chunks);
    w.u64(stats.total_readings);
    w.u64(stats.total_bytes);
    w.f64(stats.total_min);
    w.f64(stats.total_max);
    return w.finish();
}

inline bool decode_stats(const StreamBuffer& buf, PipelineStats& out) {
    BufferReader r(buf);
    if (!r.u64(out.total_chunks)) return false;
    if (!r.u64(out.total_readings)) return false;
    if (!r.u64(out.total_bytes)) return false;
    if (!r.f64(out.total_min)) return false;
    if (!r.f64(out.total_max)) return false;
    return r.done();
}

// ── Statistics Helpers ──────────────────────────────────────────────────────

inline void compute_batch_stats(const std::vector<SensorReading>& readings,
                                TransformedChunk& out) {
    if (readings.empty()) return;
    out.input_count = readings.size();
    out.min_value = readings[0].value;
    out.max_value = readings[0].value;
    double sum = 0.0;
    for (const auto& r : readings) {
        sum += r.value;
        out.min_value = std::min(out.min_value, r.value);
        out.max_value = std::max(out.max_value, r.value);
    }
    out.mean_value = sum / static_cast<double>(readings.size());

    double var_sum = 0.0;
    for (const auto& r : readings)
        var_sum += (r.value - out.mean_value) * (r.value - out.mean_value);
    out.stddev = std::sqrt(var_sum / static_cast<double>(readings.size()));
}

} // namespace hpactor::apps::streaming_pipeline
