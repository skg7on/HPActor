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

#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace hpactor::apps::edgeops_telemetry {

inline constexpr TypeTag DeviceRegisterTag{0x00030000};
inline constexpr TypeTag DeviceRegisteredTag{0x00030001};
inline constexpr TypeTag DeviceHeartbeatTag{0x00030002};
inline constexpr TypeTag TelemetryReadingTag{0x00030003};
inline constexpr TypeTag TelemetryRejectedTag{0x00030004};
inline constexpr TypeTag NormalizedReadingTag{0x00030005};
inline constexpr TypeTag WindowRollupTag{0x00030006};
inline constexpr TypeTag AlertRaisedTag{0x00030007};
inline constexpr TypeTag DeviceDisconnectedTag{0x00030008};
inline constexpr TypeTag QueryDeviceTag{0x00030009};
inline constexpr TypeTag QueryFleetSummaryTag{0x0003000A};
inline constexpr TypeTag ScenarioCommandTag{0x0003000B};
inline constexpr TypeTag DrainRoleTag{0x0003000C};
inline constexpr TypeTag FleetSummaryTag{0x0003000D};
inline constexpr TypeTag StorageSnapshotTag{0x0003000E};
inline constexpr TypeTag RollupTickTag{0x0003000F};

enum class ScenarioKind : uint8_t {
    HappyPath = 0,
    DeviceChurn,
    MalformedTelemetry,
    Overload,
    MissingRoute,
    TimerRollup,
    ProcessorRestart,
    GracefulShutdown,
    FaultInjection,
};

enum class SensorType : uint8_t {
    Temperature = 0,
    Humidity,
    Pressure,
    Power,
};

struct DeviceRegisterPayload {
    std::string device_id;
    std::string site_id;
    SensorType sensor_type = SensorType::Temperature;
    uint64_t sequence_start = 0;
};

struct DeviceRegisteredPayload {
    std::string device_id;
    uint64_t sequence_baseline = 0;
};

struct TelemetryReadingPayload {
    std::string device_id;
    std::string site_id;
    SensorType sensor_type = SensorType::Temperature;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    int64_t reading_milli = 0;
    uint32_t quality_flags = 0;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct TelemetryRejectedPayload {
    std::string device_id;
    uint64_t sequence = 0;
    std::string reason;
};

struct NormalizedReadingPayload {
    std::string device_id;
    std::string site_id;
    SensorType sensor_type = SensorType::Temperature;
    uint64_t sequence = 0;
    uint64_t timestamp_ns = 0;
    int64_t reading_milli = 0;
    uint32_t quality_flags = 0;
    ScenarioKind scenario = ScenarioKind::HappyPath;
};

struct WindowRollupPayload {
    std::string site_id;
    SensorType sensor_type = SensorType::Temperature;
    uint64_t window_start_ns = 0;
    uint64_t window_end_ns = 0;
    uint32_t count = 0;
    int64_t min_milli = 0;
    int64_t max_milli = 0;
    int64_t sum_milli = 0;
    int64_t average_milli = 0;
};

struct AlertRaisedPayload {
    std::string device_id;
    std::string site_id;
    SensorType sensor_type = SensorType::Temperature;
    uint64_t sequence = 0;
    int64_t reading_milli = 0;
    int64_t threshold_milli = 0;
    std::string reason;
};

struct QueryDevicePayload {
    std::string device_id;
};

struct FleetSummaryPayload {
    uint32_t devices_registered = 0;
    uint32_t devices_disconnected = 0;
    uint32_t readings_received = 0;
    uint32_t readings_normalized = 0;
    uint32_t readings_rejected = 0;
    uint32_t readings_stored = 0;
    uint32_t readings_dropped = 0;
    uint32_t rollups_emitted = 0;
    uint32_t alerts_raised = 0;
};

struct ScenarioCommandPayload {
    ScenarioKind scenario = ScenarioKind::HappyPath;
    uint32_t device_count = 1;
    uint32_t readings_per_device = 1;
    uint32_t rate_per_second = 0;
};

struct DrainRolePayload {
    std::string role;
    uint32_t timeout_ms = 5000;
};

class BufferWriter {
  public:
    void u8(uint8_t value) {
        buffer_.push_back(value);
    }

    void u32(uint32_t value) {
        for (int shift = 24; shift >= 0; shift -= 8)
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void u64(uint64_t value) {
        for (int shift = 56; shift >= 0; shift -= 8)
            buffer_.push_back(static_cast<uint8_t>((value >> shift) & 0xFF));
    }

    void i64(int64_t value) {
        u64(static_cast<uint64_t>(value));
    }

    void str(const std::string& value) {
        if (value.size() > std::numeric_limits<uint32_t>::max())
            return;
        u32(static_cast<uint32_t>(value.size()));
        auto begin = reinterpret_cast<const uint8_t*>(value.data());
        buffer_.insert(buffer_.end(), begin, begin + value.size());
    }

    StreamBuffer finish() {
        return std::move(buffer_);
    }

  private:
    StreamBuffer buffer_;
};

class BufferReader {
  public:
    explicit BufferReader(const StreamBuffer& buffer) : buffer_(buffer) {}

    bool u8(uint8_t& value) {
        if (offset_ + 1 > buffer_.size())
            return false;
        value = buffer_[offset_++];
        return true;
    }

    bool u32(uint32_t& value) {
        if (offset_ + 4 > buffer_.size())
            return false;
        value = 0;
        for (int i = 0; i < 4; ++i)
            value = (value << 8) | buffer_[offset_++];
        return true;
    }

    bool u64(uint64_t& value) {
        if (offset_ + 8 > buffer_.size())
            return false;
        value = 0;
        for (int i = 0; i < 8; ++i)
            value = (value << 8) | buffer_[offset_++];
        return true;
    }

    bool i64(int64_t& value) {
        uint64_t raw = 0;
        if (!u64(raw))
            return false;
        value = static_cast<int64_t>(raw);
        return true;
    }

    bool str(std::string& value) {
        uint32_t size = 0;
        if (!u32(size))
            return false;
        if (offset_ + size > buffer_.size())
            return false;
        value.assign(reinterpret_cast<const char*>(buffer_.data() + offset_), size);
        offset_ += size;
        return true;
    }

    bool done() const {
        return offset_ == buffer_.size();
    }

  private:
    const StreamBuffer& buffer_;
    size_t offset_ = 0;
};

inline const char* to_string(ScenarioKind value) {
    switch (value) {
        case ScenarioKind::HappyPath:
            return "happy-path";
        case ScenarioKind::DeviceChurn:
            return "device-churn";
        case ScenarioKind::MalformedTelemetry:
            return "malformed-telemetry";
        case ScenarioKind::Overload:
            return "overload";
        case ScenarioKind::MissingRoute:
            return "missing-route";
        case ScenarioKind::TimerRollup:
            return "timer-rollup";
        case ScenarioKind::ProcessorRestart:
            return "processor-restart";
        case ScenarioKind::GracefulShutdown:
            return "graceful-shutdown";
        case ScenarioKind::FaultInjection:
            return "fault-injection";
    }
    return "happy-path";
}

inline ScenarioKind scenario_from_string(std::string_view value) {
    if (value == "device-churn")
        return ScenarioKind::DeviceChurn;
    if (value == "malformed-telemetry")
        return ScenarioKind::MalformedTelemetry;
    if (value == "overload")
        return ScenarioKind::Overload;
    if (value == "missing-route")
        return ScenarioKind::MissingRoute;
    if (value == "timer-rollup")
        return ScenarioKind::TimerRollup;
    if (value == "processor-restart")
        return ScenarioKind::ProcessorRestart;
    if (value == "graceful-shutdown")
        return ScenarioKind::GracefulShutdown;
    if (value == "fault-injection")
        return ScenarioKind::FaultInjection;
    return ScenarioKind::HappyPath;
}

inline const char* to_string(SensorType value) {
    switch (value) {
        case SensorType::Temperature:
            return "temperature";
        case SensorType::Humidity:
            return "humidity";
        case SensorType::Pressure:
            return "pressure";
        case SensorType::Power:
            return "power";
    }
    return "temperature";
}

inline bool valid_scenario(uint8_t value) {
    return value <= static_cast<uint8_t>(ScenarioKind::FaultInjection);
}

inline bool valid_sensor(uint8_t value) {
    return value <= static_cast<uint8_t>(SensorType::Power);
}

inline StreamBuffer encode_device_register(const DeviceRegisterPayload& value) {
    BufferWriter writer;
    writer.str(value.device_id);
    writer.str(value.site_id);
    writer.u8(static_cast<uint8_t>(value.sensor_type));
    writer.u64(value.sequence_start);
    return writer.finish();
}

inline bool
decode_device_register(const StreamBuffer& buffer, DeviceRegisterPayload& value) {
    BufferReader reader(buffer);
    uint8_t sensor = 0;
    if (!reader.str(value.device_id) || !reader.str(value.site_id) ||
        !reader.u8(sensor) || !reader.u64(value.sequence_start) ||
        !reader.done() || !valid_sensor(sensor))
        return false;
    value.sensor_type = static_cast<SensorType>(sensor);
    return true;
}

inline StreamBuffer encode_device_registered(const DeviceRegisteredPayload& value) {
    BufferWriter writer;
    writer.str(value.device_id);
    writer.u64(value.sequence_baseline);
    return writer.finish();
}

inline bool decode_device_registered(const StreamBuffer& buffer,
                                     DeviceRegisteredPayload& value) {
    BufferReader reader(buffer);
    return reader.str(value.device_id) && reader.u64(value.sequence_baseline) &&
           reader.done();
}

inline StreamBuffer encode_telemetry_reading(const TelemetryReadingPayload& value) {
    BufferWriter writer;
    writer.str(value.device_id);
    writer.str(value.site_id);
    writer.u8(static_cast<uint8_t>(value.sensor_type));
    writer.u64(value.sequence);
    writer.u64(value.timestamp_ns);
    writer.i64(value.reading_milli);
    writer.u32(value.quality_flags);
    writer.u8(static_cast<uint8_t>(value.scenario));
    return writer.finish();
}

inline bool decode_telemetry_reading(const StreamBuffer& buffer,
                                     TelemetryReadingPayload& value) {
    BufferReader reader(buffer);
    uint8_t sensor = 0;
    uint8_t scenario = 0;
    if (!reader.str(value.device_id) || !reader.str(value.site_id) ||
        !reader.u8(sensor) || !reader.u64(value.sequence) ||
        !reader.u64(value.timestamp_ns) || !reader.i64(value.reading_milli) ||
        !reader.u32(value.quality_flags) || !reader.u8(scenario) ||
        !reader.done() || !valid_sensor(sensor) || !valid_scenario(scenario))
        return false;
    value.sensor_type = static_cast<SensorType>(sensor);
    value.scenario = static_cast<ScenarioKind>(scenario);
    return true;
}

inline StreamBuffer
encode_normalized_reading(const NormalizedReadingPayload& value) {
    TelemetryReadingPayload reading{value.device_id,     value.site_id,
                                    value.sensor_type,   value.sequence,
                                    value.timestamp_ns,  value.reading_milli,
                                    value.quality_flags, value.scenario};
    return encode_telemetry_reading(reading);
}

inline bool decode_normalized_reading(const StreamBuffer& buffer,
                                      NormalizedReadingPayload& value) {
    TelemetryReadingPayload reading;
    if (!decode_telemetry_reading(buffer, reading))
        return false;
    value = NormalizedReadingPayload{
        reading.device_id,     reading.site_id,      reading.sensor_type,
        reading.sequence,      reading.timestamp_ns, reading.reading_milli,
        reading.quality_flags, reading.scenario,
    };
    return true;
}

inline StreamBuffer
encode_telemetry_rejected(const TelemetryRejectedPayload& value) {
    BufferWriter writer;
    writer.str(value.device_id);
    writer.u64(value.sequence);
    writer.str(value.reason);
    return writer.finish();
}

inline bool decode_telemetry_rejected(const StreamBuffer& buffer,
                                      TelemetryRejectedPayload& value) {
    BufferReader reader(buffer);
    return reader.str(value.device_id) && reader.u64(value.sequence) &&
           reader.str(value.reason) && reader.done();
}

inline StreamBuffer encode_window_rollup(const WindowRollupPayload& value) {
    BufferWriter writer;
    writer.str(value.site_id);
    writer.u8(static_cast<uint8_t>(value.sensor_type));
    writer.u64(value.window_start_ns);
    writer.u64(value.window_end_ns);
    writer.u32(value.count);
    writer.i64(value.min_milli);
    writer.i64(value.max_milli);
    writer.i64(value.sum_milli);
    writer.i64(value.average_milli);
    return writer.finish();
}

inline bool
decode_window_rollup(const StreamBuffer& buffer, WindowRollupPayload& value) {
    BufferReader reader(buffer);
    uint8_t sensor = 0;
    if (!reader.str(value.site_id) || !reader.u8(sensor) ||
        !reader.u64(value.window_start_ns) || !reader.u64(value.window_end_ns) ||
        !reader.u32(value.count) || !reader.i64(value.min_milli) ||
        !reader.i64(value.max_milli) || !reader.i64(value.sum_milli) ||
        !reader.i64(value.average_milli) || !reader.done() || !valid_sensor(sensor))
        return false;
    value.sensor_type = static_cast<SensorType>(sensor);
    return true;
}

inline StreamBuffer encode_alert_raised(const AlertRaisedPayload& value) {
    BufferWriter writer;
    writer.str(value.device_id);
    writer.str(value.site_id);
    writer.u8(static_cast<uint8_t>(value.sensor_type));
    writer.u64(value.sequence);
    writer.i64(value.reading_milli);
    writer.i64(value.threshold_milli);
    writer.str(value.reason);
    return writer.finish();
}

inline bool
decode_alert_raised(const StreamBuffer& buffer, AlertRaisedPayload& value) {
    BufferReader reader(buffer);
    uint8_t sensor = 0;
    if (!reader.str(value.device_id) || !reader.str(value.site_id) ||
        !reader.u8(sensor) || !reader.u64(value.sequence) ||
        !reader.i64(value.reading_milli) || !reader.i64(value.threshold_milli) ||
        !reader.str(value.reason) || !reader.done() || !valid_sensor(sensor))
        return false;
    value.sensor_type = static_cast<SensorType>(sensor);
    return true;
}

inline StreamBuffer encode_query_device(const QueryDevicePayload& value) {
    BufferWriter writer;
    writer.str(value.device_id);
    return writer.finish();
}

inline bool
decode_query_device(const StreamBuffer& buffer, QueryDevicePayload& value) {
    BufferReader reader(buffer);
    return reader.str(value.device_id) && reader.done();
}

inline StreamBuffer encode_fleet_summary(const FleetSummaryPayload& value) {
    BufferWriter writer;
    writer.u32(value.devices_registered);
    writer.u32(value.devices_disconnected);
    writer.u32(value.readings_received);
    writer.u32(value.readings_normalized);
    writer.u32(value.readings_rejected);
    writer.u32(value.readings_stored);
    writer.u32(value.readings_dropped);
    writer.u32(value.rollups_emitted);
    writer.u32(value.alerts_raised);
    return writer.finish();
}

inline bool
decode_fleet_summary(const StreamBuffer& buffer, FleetSummaryPayload& value) {
    BufferReader reader(buffer);
    return reader.u32(value.devices_registered) &&
           reader.u32(value.devices_disconnected) &&
           reader.u32(value.readings_received) &&
           reader.u32(value.readings_normalized) &&
           reader.u32(value.readings_rejected) &&
           reader.u32(value.readings_stored) &&
           reader.u32(value.readings_dropped) &&
           reader.u32(value.rollups_emitted) &&
           reader.u32(value.alerts_raised) && reader.done();
}

inline StreamBuffer encode_scenario_command(const ScenarioCommandPayload& value) {
    BufferWriter writer;
    writer.u8(static_cast<uint8_t>(value.scenario));
    writer.u32(value.device_count);
    writer.u32(value.readings_per_device);
    writer.u32(value.rate_per_second);
    return writer.finish();
}

inline bool decode_scenario_command(const StreamBuffer& buffer,
                                    ScenarioCommandPayload& value) {
    BufferReader reader(buffer);
    uint8_t scenario = 0;
    if (!reader.u8(scenario) || !reader.u32(value.device_count) ||
        !reader.u32(value.readings_per_device) ||
        !reader.u32(value.rate_per_second) || !reader.done() ||
        !valid_scenario(scenario))
        return false;
    value.scenario = static_cast<ScenarioKind>(scenario);
    return true;
}

inline StreamBuffer encode_drain_role(const DrainRolePayload& value) {
    BufferWriter writer;
    writer.str(value.role);
    writer.u32(value.timeout_ms);
    return writer.finish();
}

inline bool decode_drain_role(const StreamBuffer& buffer, DrainRolePayload& value) {
    BufferReader reader(buffer);
    return reader.str(value.role) && reader.u32(value.timeout_ms) && reader.done();
}

} // namespace hpactor::apps::edgeops_telemetry
